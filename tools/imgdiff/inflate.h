/* A from-scratch RFC 1951 (DEFLATE) decompressor -- stored, fixed-Huffman, and dynamic-Huffman
 * blocks (tools/imgdiff has no third-party codec dependency, ARCHITECTURE §0). */
#ifndef IMGDIFF_INFLATE_H
#define IMGDIFF_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decompresses the raw DEFLATE stream `data[0..size)` into `out` (capacity `outCap`), setting
 * `*outLen` to the number of bytes produced. The caller must know the exact expected output size
 * up front (PNG's IHDR gives it) and pass that as `outCap` -- decoding fails (false) rather than
 * growing the buffer if the stream would produce more. Also fails on any malformed input
 * (truncated stream, an invalid Huffman code, a back-reference before the start of output, a
 * reserved block type). */
bool inflateRaw(const uint8_t *data, size_t size, uint8_t *out, size_t outCap, size_t *outLen);

#endif
