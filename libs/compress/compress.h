/* libs/compress: from-scratch DEFLATE (RFC 1951) and zlib (RFC 1950) codecs, Adler-32 and CRC-32
 * (D-140). Shared by libs/gfx (PNG), tools/imgdiff, and later the package and TLS code (M11.4).
 * Every entry point works on caller-provided buffers, never allocates (Huffman tables live on the
 * stack), is pure and reentrant, and has no libc dependency beyond <string.h>.
 *
 * Status codes: STATUS_ERR_INVALID = malformed or truncated input, or a checksum mismatch;
 * STATUS_ERR_NO_MEMORY = the output would not fit in `outCap`. None of the names below collide
 * with zlib's own (`compress`, `compressBound`), since ported code links a real zlib. */
#ifndef LIBS_COMPRESS_H
#define LIBS_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#include "uapi/status.h"

/* Decompresses the raw DEFLATE stream `in[0..inLen)` into `out` (capacity `outCap`; `out` may be
 * NULL when `outCap` is 0), setting `*outLen` to the bytes produced and `*inUsed` to the bytes of
 * `in` the stream occupied (including the final partial byte). Either out-parameter may be NULL.
 * On failure `*outLen` is the partial output and `*inUsed` is 0. Decoding stops at the
 * final block; trailing input is left alone. Rejects over-subscribed and (except the single-code
 * case zlib allows) incomplete Huffman codes, hlit > 286, hdist > 30, and a dynamic block with no
 * end-of-block code. */
Status compressInflateRaw(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap,
                          size_t *outLen, size_t *inUsed);

/* zlib format: validates the 2-byte header (CM=8, CINFO<=7, FCHECK, no preset dictionary),
 * inflates, and verifies the Adler-32 that follows the DEFLATE stream (read at in[2 + rawUsed], not
 * at the end of `in`, so a caller may pass a buffer with trailing bytes). `*inUsed` counts the
 * whole zlib stream, trailer included. On failure neither out-parameter is written. */
Status compressZlibInflate(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap,
                           size_t *outLen, size_t *inUsed);

/* A single fixed-Huffman block with a restricted greedy LZ77 (two candidate distances: 3, and
 * `rowStride` if non-zero, e.g. one image row up). NO_MEMORY if it would exceed `outCap`.
 * Unlike the inflate functions, `outLen` must not be NULL. */
Status compressDeflateFixed(const uint8_t *in, size_t inLen, uint32_t rowStride, uint8_t *out,
                            size_t outCap, size_t *outLen);

/* "78 01" header + compressDeflateFixed + big-endian Adler-32. `outLen` must not be NULL. */
Status compressZlibDeflate(const uint8_t *in, size_t inLen, uint32_t rowStride, uint8_t *out,
                           size_t outCap, size_t *outLen);

/* Running Adler-32: start with `adler` = 1. */
uint32_t compressAdler32(uint32_t adler, const uint8_t *p, size_t n);

/* Running CRC-32 (IEEE, reflected; zlib's `crc32()` convention): start with `crc` = 0, and pass
 * the previous return value to continue. */
uint32_t compressCrc32(uint32_t crc, const void *p, size_t n);

#endif
