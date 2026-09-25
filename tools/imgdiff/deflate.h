/* A from-scratch RFC 1951 (DEFLATE) compressor: a single fixed-Huffman block (§3.2.6) with a
 * restricted greedy LZ77 that only tries two candidate back-reference distances per position --
 * 3 (the previous pixel, for a PNG filter-0 RGB stream) and the caller-supplied `rowStride` (the
 * same pixel one row up) -- rather than a general sliding-window search. This is much simpler
 * than a real compressor and still gets real compression on the flat, repetitive synthetic images
 * this tool exists to diff (boot menus, glyph test cards), per D-070. */
#ifndef IMGDIFF_DEFLATE_H
#define IMGDIFF_DEFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Compresses `data[0..size)` into `out` (capacity `outCap`), setting `*outLen`. `rowStride` is
 * the second candidate back-reference distance (0 disables it, e.g. for non-image data). Returns
 * false if the output would exceed `outCap` (the caller should size it generously -- this
 * encoder never expands data by more than a small constant per byte in the worst case, since it
 * always falls back to a literal). */
bool deflateFixed(const uint8_t *data, size_t size, uint32_t rowStride, uint8_t *out, size_t outCap,
                  size_t *outLen);

#endif
