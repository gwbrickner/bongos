/* A from-scratch PNG reader/writer (ARCHITECTURE §0/§23, D-070): 8-bit-depth, non-interlaced,
 * color type 2 (RGB) or 6 (RGBA, alpha must be all-255) on read; always writes color type 2. No
 * third-party PNG/zlib library -- built on this tool's own inflate.c/deflate.c. */
#ifndef IMGDIFF_PNG_H
#define IMGDIFF_PNG_H

#include <stdbool.h>
#include <stddef.h>

#include "image.h"

bool pngRead(const char *path, Image *out, char *errbuf, size_t errbufCap);
bool pngWrite(const char *path, const Image *img, char *errbuf, size_t errbufCap);

#endif
