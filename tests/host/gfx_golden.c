/* See gfx_golden.h. */
#define _DEFAULT_SOURCE /* mkdir */
#include "gfx_golden.h"

#include "compare.h"
#include "image.h"
#include "png.h"

#include <stdio.h>
#include <sys/stat.h>

#define REF_DIR "tests/data/gfx/ref"
#define OUT_DIR "build/host-tests/gfx-out"

static void ensureOutDir(void) {
    mkdir("build", 0755);
    mkdir("build/host-tests", 0755);
    mkdir(OUT_DIR, 0755);
}

static bool compareImage(const char *name, const Image *actual) {
    char refPath[256], outPath[256], diffPath[256], err[256];
    snprintf(refPath, sizeof(refPath), REF_DIR "/%s.png", name);
    snprintf(outPath, sizeof(outPath), OUT_DIR "/%s.png", name);
    snprintf(diffPath, sizeof(diffPath), OUT_DIR "/%s.diff.png", name);

    Image ref;
    if (!pngRead(refPath, &ref, err, sizeof(err))) {
        ensureOutDir();
        pngWrite(outPath, actual, err, sizeof(err));
        fprintf(stderr, "  golden '%s': no usable reference (%s); actual image written to %s\n",
                name, refPath, outPath);
        return false;
    }
    CompareResult res;
    bool match = imagesCompare(actual, &ref, 0, 0, NULL, &res);
    if (!match) {
        ensureOutDir();
        pngWrite(outPath, actual, err, sizeof(err));
        if (res.dimensionsMatch) {
            writeDiffImage(diffPath, actual, &ref, NULL, 0, err, sizeof(err));
        }
        fprintf(stderr,
                "  golden '%s' MISMATCH: dims %s, %u differing pixels (first at %u,%u, max delta "
                "%u); actual %s, diff %s\n",
                name, res.dimensionsMatch ? "match" : "DIFFER", res.diffPixelCount, res.firstDiffX,
                res.firstDiffY, res.maxDeltaSeen, outPath, diffPath);
    }
    imageFree(&ref);
    return match;
}

bool goldenCheck(const char *name, const GfxSurface *s) {
    Image img;
    if (!imageAlloc(&img, (uint32_t)s->width, (uint32_t)s->height)) {
        return false;
    }
    bool opaque = true;
    for (int32_t y = 0; y < s->height; y++) {
        for (int32_t x = 0; x < s->width; x++) {
            uint32_t p = s->pixels[(size_t)y * (size_t)s->stride + (size_t)x];
            if ((p >> 24) != 0xFFu) {
                opaque = false;
            }
            uint8_t *d = img.rgb + ((size_t)y * (size_t)s->width + (size_t)x) * 3;
            d[0] = (uint8_t)(p >> 16);
            d[1] = (uint8_t)(p >> 8);
            d[2] = (uint8_t)p;
        }
    }
    if (!opaque) {
        fprintf(stderr, "  golden '%s': surface has non-opaque pixels; paint a background first\n",
                name);
    }
    bool ok = opaque && compareImage(name, &img);
    imageFree(&img);
    return ok;
}

bool goldenCheckMask(const char *name, const GfxMask *m) {
    Image img;
    if (!imageAlloc(&img, (uint32_t)m->width, (uint32_t)m->height)) {
        return false;
    }
    for (int32_t y = 0; y < m->height; y++) {
        for (int32_t x = 0; x < m->width; x++) {
            uint8_t v = m->data[(size_t)y * (size_t)m->stride + (size_t)x];
            uint8_t *d = img.rgb + ((size_t)y * (size_t)m->width + (size_t)x) * 3;
            d[0] = d[1] = d[2] = v;
        }
    }
    bool ok = compareImage(name, &img);
    imageFree(&img);
    return ok;
}
