/* Pixel comparison, mask-file parsing, and diff-image rendering for `imgdiff compare`
 * (ARCHITECTURE §23/D-070). */
#ifndef IMGDIFF_COMPARE_H
#define IMGDIFF_COMPARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "image.h"

typedef struct {
    uint32_t x, y, w, h;
} MaskRect;

typedef struct {
    MaskRect *rects;
    uint32_t count;
} MaskList;

/* Parses `x y w h` (decimal, whitespace-separated) per non-blank, non-'#'-comment line, clipping
 * each rect to `imgWidth`x`imgHeight`. False (with `errbuf` set) on a malformed line. */
bool maskLoad(const char *path, uint32_t imgWidth, uint32_t imgHeight, MaskList *out, char *errbuf,
              size_t errbufCap);
void maskFree(MaskList *m);
bool maskContains(const MaskList *m, uint32_t x, uint32_t y);

typedef struct {
    bool dimensionsMatch;
    uint32_t diffPixelCount; /* outside the mask, exceeding maxChannelDelta */
    uint32_t maxDeltaSeen;
    uint32_t firstDiffX, firstDiffY;
    bool haveFirstDiff;
} CompareResult;

/* Compares `actual` against `ref` pixel by pixel (skipping masked pixels entirely): a pixel
 * counts as differing if max(|dR|,|dG|,|dB|) > maxChannelDelta. Returns true (a match) iff the
 * dimensions are equal and diffPixelCount <= maxDiffPixels. Always fills `result` with the full
 * diagnostics, even on a match. */
bool imagesCompare(const Image *actual, const Image *ref, int maxChannelDelta,
                   uint32_t maxDiffPixels, const MaskList *mask, CompareResult *result);

/* Writes a diagnostic PNG: matching pixels at 25% intensity, differing pixels FF0000, masked
 * pixels 000040 -- same rule imagesCompare() used to produce `result`. Requires matching
 * dimensions. */
bool writeDiffImage(const char *path, const Image *actual, const Image *ref, const MaskList *mask,
                    int maxChannelDelta, char *errbuf, size_t errbufCap);

#endif
