/* The in-memory image type shared by every tools/imgdiff format reader/writer: 8-bit RGB,
 * row-major, no padding between rows. ARCHITECTURE §23/D-070. */
#ifndef IMGDIFF_IMAGE_H
#define IMGDIFF_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#define IMGDIFF_MAX_DIM 16384u /* matches the GOP width/height cap, D-068 */

typedef struct {
    uint32_t width, height;
    uint8_t *rgb; /* width*height*3 bytes, owned; NULL if unallocated */
} Image;

/* Allocates `img->rgb` (uninitialized) for `width`x`height`; false (img left zeroed) on a bad
 * size (0, over IMGDIFF_MAX_DIM, or a width*height*3 that would overflow size_t) or an allocation
 * failure. */
bool imageAlloc(Image *img, uint32_t width, uint32_t height);

/* Frees img->rgb (NULL-safe) and zeroes *img. */
void imageFree(Image *img);

#endif
