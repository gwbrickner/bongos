/* A minimal RFC 1950 (zlib format) wrapper around inflate.c/deflate.c: the 2-byte header, the
 * raw DEFLATE stream, and the 4-byte big-endian Adler-32 trailer PNG's IDAT chunks use. */
#ifndef IMGDIFF_ZLIB_WRAP_H
#define IMGDIFF_ZLIB_WRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t adler32Compute(const uint8_t *data, size_t len);

/* Validates the header (CM=8, FCHECK, FDICT=0 -- a preset dictionary isn't supported), inflates
 * the DEFLATE payload into `out` (capacity `outCap`), and verifies the trailing Adler-32 against
 * the decompressed data. False on any of those failing. */
bool zlibInflate(const uint8_t *data, size_t size, uint8_t *out, size_t outCap, size_t *outLen);

/* Writes a fixed "78 01" header (CM=8/CINFO=7, a valid FCHECK, FDICT=0), the fixed-Huffman
 * DEFLATE payload (via deflateFixed, `rowStride` passed through), and the big-endian Adler-32
 * trailer, into `out` (capacity `outCap`). False if it wouldn't fit. */
bool zlibDeflate(const uint8_t *data, size_t size, uint32_t rowStride, uint8_t *out, size_t outCap,
                 size_t *outLen);

#endif
