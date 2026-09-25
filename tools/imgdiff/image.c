#include "image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool imageAlloc(Image *img, uint32_t width, uint32_t height) {
    img->width = 0;
    img->height = 0;
    img->rgb = NULL;
    if (width == 0 || height == 0 || width > IMGDIFF_MAX_DIM || height > IMGDIFF_MAX_DIM) {
        return false;
    }
    uint64_t total = (uint64_t)width * (uint64_t)height * 3u;
    if (total > (uint64_t)SIZE_MAX) {
        return false;
    }
    uint8_t *buf = malloc((size_t)total);
    if (buf == NULL) {
        return false;
    }
    img->width = width;
    img->height = height;
    img->rgb = buf;
    return true;
}

void imageFree(Image *img) {
    free(img->rgb);
    img->rgb = NULL;
    img->width = 0;
    img->height = 0;
}
