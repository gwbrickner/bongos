/* See png.h. PNG chunk framing per the W3C PNG spec: an 8-byte signature, then a sequence of
 * length-prefixed, CRC-32-trailed chunks (type IHDR first, IEND last). Filtering per the spec's
 * "Filtering" section (five per-scanline filter types, Paeth included). */
#include "png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"
#include "zlib_wrap.h"

static const uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

static void setErr(char *errbuf, size_t errbufCap, const char *msg) {
    if (errbuf != NULL && errbufCap > 0) {
        size_t i = 0;
        for (; msg[i] != '\0' && i < errbufCap - 1; i++) {
            errbuf[i] = msg[i];
        }
        errbuf[i] = '\0';
    }
}

static uint32_t readBE32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void writeBE32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- reading ---- */

typedef struct {
    uint8_t *data;
    size_t len, cap;
} ByteBuf;

static bool bbAppend(ByteBuf *bb, const uint8_t *data, size_t len) {
    if (bb->len + len > bb->cap) {
        size_t newCap = bb->cap == 0 ? 4096 : bb->cap * 2;
        while (newCap < bb->len + len) {
            newCap *= 2;
        }
        uint8_t *n = realloc(bb->data, newCap);
        if (n == NULL) {
            return false;
        }
        bb->data = n;
        bb->cap = newCap;
    }
    memcpy(bb->data + bb->len, data, len);
    bb->len += len;
    return true;
}

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) {
        return (uint8_t)a;
    }
    if (pb <= pc) {
        return (uint8_t)b;
    }
    return (uint8_t)c;
}

/* Defilters `raw` in place: `raw` is `height` scanlines of (1 filter-type byte + width*bpp pixel
 * bytes). Each row's filter is undone using the *already-defiltered* previous row (rows are
 * processed in order, overwriting each row's pixel bytes with the reconstructed values before
 * moving to the next, so "previous row" always means "reconstructed", never "still filtered"). */
static bool pngDefilter(uint8_t *raw, uint32_t width, uint32_t height, uint32_t bpp) {
    uint32_t rowBytes = width * bpp;
    uint32_t stride = 1 + rowBytes;
    for (uint32_t r = 0; r < height; r++) {
        uint8_t filterType = raw[(size_t)r * stride];
        uint8_t *cur = raw + (size_t)r * stride + 1;
        const uint8_t *prev = (r == 0) ? NULL : raw + (size_t)(r - 1) * stride + 1;
        for (uint32_t i = 0; i < rowBytes; i++) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = (prev != NULL) ? prev[i] : 0;
            int c = (prev != NULL && i >= bpp) ? prev[i - bpp] : 0;
            switch (filterType) {
                case 0:
                    break;
                case 1:
                    cur[i] = (uint8_t)(cur[i] + a);
                    break;
                case 2:
                    cur[i] = (uint8_t)(cur[i] + b);
                    break;
                case 3:
                    cur[i] = (uint8_t)(cur[i] + (uint8_t)((a + b) / 2));
                    break;
                case 4:
                    cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
                    break;
                default:
                    return false;
            }
        }
    }
    return true;
}

bool pngRead(const char *path, Image *out, char *errbuf, size_t errbufCap) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        setErr(errbuf, errbufCap, "cannot open file");
        return false;
    }
    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, PNG_SIGNATURE, 8) != 0) {
        setErr(errbuf, errbufCap, "not a PNG file (bad signature)");
        fclose(f);
        return false;
    }

    bool haveIhdr = false, haveIend = false;
    uint32_t width = 0, height = 0;
    uint8_t colorType = 0;
    ByteBuf idat = {0};

    while (!haveIend) {
        uint8_t lenBuf[4], typeBuf[4];
        if (fread(lenBuf, 1, 4, f) != 4 || fread(typeBuf, 1, 4, f) != 4) {
            setErr(errbuf, errbufCap, "truncated chunk header");
            goto fail;
        }
        uint32_t chunkLen = readBE32(lenBuf);
        if (chunkLen > 64u * 1024u * 1024u) {
            setErr(errbuf, errbufCap, "chunk too large");
            goto fail;
        }
        uint8_t *chunkData = NULL;
        if (chunkLen > 0) {
            chunkData = malloc(chunkLen);
            if (chunkData == NULL || fread(chunkData, 1, chunkLen, f) != chunkLen) {
                setErr(errbuf, errbufCap, "truncated chunk data");
                free(chunkData);
                goto fail;
            }
        }
        uint8_t crcBuf[4];
        if (fread(crcBuf, 1, 4, f) != 4) {
            setErr(errbuf, errbufCap, "truncated chunk CRC");
            free(chunkData);
            goto fail;
        }
        uint8_t *crcInput = malloc((size_t)chunkLen + 4);
        if (crcInput == NULL) {
            setErr(errbuf, errbufCap, "out of memory");
            free(chunkData);
            goto fail;
        }
        memcpy(crcInput, typeBuf, 4);
        if (chunkLen > 0) {
            memcpy(crcInput + 4, chunkData, chunkLen);
        }
        uint32_t computedCrc = crc32Compute(crcInput, (size_t)chunkLen + 4);
        free(crcInput);
        if (computedCrc != readBE32(crcBuf)) {
            setErr(errbuf, errbufCap, "chunk CRC mismatch");
            free(chunkData);
            goto fail;
        }

        if (memcmp(typeBuf, "IHDR", 4) == 0) {
            if (haveIhdr || chunkLen != 13) {
                setErr(errbuf, errbufCap, "malformed IHDR");
                free(chunkData);
                goto fail;
            }
            width = readBE32(chunkData);
            height = readBE32(chunkData + 4);
            uint8_t bitDepth = chunkData[8];
            colorType = chunkData[9];
            uint8_t compression = chunkData[10];
            uint8_t filterMethod = chunkData[11];
            uint8_t interlace = chunkData[12];
            if (width == 0 || height == 0 || width > IMGDIFF_MAX_DIM || height > IMGDIFF_MAX_DIM ||
                bitDepth != 8 || (colorType != 2 && colorType != 6) || compression != 0 ||
                filterMethod != 0 || interlace != 0) {
                setErr(errbuf, errbufCap, "unsupported PNG (need 8-bit, non-interlaced, RGB/RGBA)");
                free(chunkData);
                goto fail;
            }
            haveIhdr = true;
        } else if (memcmp(typeBuf, "IDAT", 4) == 0) {
            if (!haveIhdr) {
                setErr(errbuf, errbufCap, "IDAT before IHDR");
                free(chunkData);
                goto fail;
            }
            if (chunkLen > 0 && !bbAppend(&idat, chunkData, chunkLen)) {
                setErr(errbuf, errbufCap, "out of memory");
                free(chunkData);
                goto fail;
            }
        } else if (memcmp(typeBuf, "IEND", 4) == 0) {
            haveIend = true;
        } else if (memcmp(typeBuf, "PLTE", 4) == 0) {
            /* Ignored: only RGB/RGBA color types are supported, neither needs a palette. */
        } else if ((typeBuf[0] & 0x20) == 0) {
            /* Uppercase first letter: a critical chunk we don't understand. */
            setErr(errbuf, errbufCap, "unknown critical PNG chunk");
            free(chunkData);
            goto fail;
        } /* else: a known-safe-to-skip ancillary chunk (tEXt, pHYs, ...) */
        free(chunkData);
    }
    fclose(f);
    f = NULL;

    if (!haveIhdr) {
        setErr(errbuf, errbufCap, "missing IHDR");
        goto failNoFile;
    }

    uint32_t bpp = (colorType == 6) ? 4u : 3u;
    size_t rawLen = (size_t)height * (1 + (size_t)width * bpp);
    uint8_t *raw = malloc(rawLen);
    if (raw == NULL) {
        setErr(errbuf, errbufCap, "out of memory");
        goto failNoFile;
    }
    size_t inflatedLen = 0;
    if (!zlibInflate(idat.data, idat.len, raw, rawLen, &inflatedLen) || inflatedLen != rawLen) {
        setErr(errbuf, errbufCap, "corrupt or truncated PNG image data");
        free(raw);
        goto failNoFile;
    }
    free(idat.data);
    idat.data = NULL;

    if (!pngDefilter(raw, width, height, bpp)) {
        setErr(errbuf, errbufCap, "unsupported PNG filter type");
        free(raw);
        return false;
    }

    if (!imageAlloc(out, width, height)) {
        setErr(errbuf, errbufCap, "out of memory");
        free(raw);
        return false;
    }
    uint32_t stride = 1 + width * bpp;
    for (uint32_t r = 0; r < height; r++) {
        const uint8_t *row = raw + (size_t)r * stride + 1;
        uint8_t *dstRow = out->rgb + (size_t)r * width * 3;
        for (uint32_t c = 0; c < width; c++) {
            dstRow[c * 3 + 0] = row[c * bpp + 0];
            dstRow[c * 3 + 1] = row[c * bpp + 1];
            dstRow[c * 3 + 2] = row[c * bpp + 2];
            if (bpp == 4 && row[c * bpp + 3] != 255) {
                setErr(errbuf, errbufCap, "RGBA PNG has non-opaque pixels (unsupported)");
                free(raw);
                imageFree(out);
                return false;
            }
        }
    }
    free(raw);
    return true;

fail:
    free(idat.data);
    if (f != NULL) {
        fclose(f);
    }
    return false;
failNoFile:
    free(idat.data);
    return false;
}

/* ---- writing ---- */

static bool writeChunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t lenBuf[4];
    writeBE32(lenBuf, len);
    if (fwrite(lenBuf, 1, 4, f) != 4 || fwrite(type, 1, 4, f) != 4) {
        return false;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        return false;
    }
    uint8_t *crcInput = malloc((size_t)len + 4);
    if (crcInput == NULL) {
        return false;
    }
    memcpy(crcInput, type, 4);
    if (len > 0) {
        memcpy(crcInput + 4, data, len);
    }
    uint32_t crc = crc32Compute(crcInput, (size_t)len + 4);
    free(crcInput);
    uint8_t crcBuf[4];
    writeBE32(crcBuf, crc);
    return fwrite(crcBuf, 1, 4, f) == 4;
}

bool pngWrite(const char *path, const Image *img, char *errbuf, size_t errbufCap) {
    uint32_t bpp = 3;
    uint32_t stride = 1 + img->width * bpp;
    size_t rawLen = (size_t)img->height * stride;
    uint8_t *raw = malloc(rawLen);
    if (raw == NULL) {
        setErr(errbuf, errbufCap, "out of memory");
        return false;
    }
    for (uint32_t r = 0; r < img->height; r++) {
        raw[(size_t)r * stride] = 0; /* filter type 0 (None) on every row, D-070 */
        memcpy(raw + (size_t)r * stride + 1, img->rgb + (size_t)r * img->width * 3,
               (size_t)img->width * 3);
    }

    /* Generous bound: this encoder never expands data (every literal costs exactly the fixed
     * Huffman code for that byte, at most 9 bits, versus 8 bits raw -- so worst case is close to
     * 9/8 the input size), plus zlib's 6-byte header/trailer and slack for the End-of-block code
     * and byte-alignment padding. */
    size_t zlibCap = rawLen + rawLen / 4 + 64;
    uint8_t *zlibBuf = malloc(zlibCap);
    if (zlibBuf == NULL) {
        setErr(errbuf, errbufCap, "out of memory");
        free(raw);
        return false;
    }
    size_t zlibLen = 0;
    if (!zlibDeflate(raw, rawLen, stride, zlibBuf, zlibCap, &zlibLen)) {
        setErr(errbuf, errbufCap, "compression failed (image too large?)");
        free(raw);
        free(zlibBuf);
        return false;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        setErr(errbuf, errbufCap, "cannot create file");
        free(zlibBuf);
        return false;
    }
    bool ok = fwrite(PNG_SIGNATURE, 1, 8, f) == 8;

    uint8_t ihdr[13];
    writeBE32(ihdr, img->width);
    writeBE32(ihdr + 4, img->height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* color type: RGB */
    ihdr[10] = 0; /* compression method */
    ihdr[11] = 0; /* filter method */
    ihdr[12] = 0; /* interlace method */
    ok = ok && writeChunk(f, "IHDR", ihdr, sizeof(ihdr));
    ok = ok && writeChunk(f, "IDAT", zlibBuf, (uint32_t)zlibLen);
    ok = ok && writeChunk(f, "IEND", NULL, 0);
    free(zlibBuf);
    fclose(f);
    if (!ok) {
        setErr(errbuf, errbufCap, "write error");
    }
    return ok;
}
