/* Netpbm PPM (P6, binary), the format QEMU's QMP `screendump` writes (ARCHITECTURE §23/D-070). */
#ifndef IMGDIFF_PPM_H
#define IMGDIFF_PPM_H

#include <stdbool.h>
#include <stddef.h>

#include "image.h"

/* Reads a P6 PPM (maxval must be 255) from `path` into `out`. `errbuf` (capacity `errbufCap`)
 * gets a human-readable reason on failure; pass NULL/0 to skip that. */
bool ppmRead(const char *path, Image *out, char *errbuf, size_t errbufCap);

/* Writes `img` as a P6 PPM to `path`. */
bool ppmWrite(const char *path, const Image *img, char *errbuf, size_t errbufCap);

#endif
