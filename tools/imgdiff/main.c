/* tools/imgdiff: compares two images (PPM/PNG) with an optional tolerance and mask, or converts
 * between the two formats (ARCHITECTURE §23, ROADMAP M1.4, D-070). A host tool (own clang, no
 * cross flags), per ARCHITECTURE §0's host-tool exception. No third-party image/codec library:
 * the PNG/zlib/DEFLATE codec is this tool's own (png.c/zlib_wrap.c/inflate.c/deflate.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compare.h"
#include "image.h"
#include "png.h"
#include "ppm.h"

typedef enum { FMT_UNKNOWN, FMT_PPM, FMT_PNG } Format;

static Format formatFromMagic(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return FMT_UNKNOWN;
    }
    uint8_t buf[8];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n >= 2 && buf[0] == 'P' && buf[1] == '6') {
        return FMT_PPM;
    }
    if (n >= 8 && buf[0] == 137 && buf[1] == 80 && buf[2] == 78 && buf[3] == 71) {
        return FMT_PNG;
    }
    return FMT_UNKNOWN;
}

static Format formatFromExtension(const char *path) {
    size_t len = strlen(path);
    if (len >= 4 && strcmp(path + len - 4, ".png") == 0) {
        return FMT_PNG;
    }
    if (len >= 4 && strcmp(path + len - 4, ".ppm") == 0) {
        return FMT_PPM;
    }
    return FMT_UNKNOWN;
}

static bool loadImage(const char *path, Image *out, char *errbuf, size_t errbufCap) {
    Format fmt = formatFromMagic(path);
    if (fmt == FMT_PPM) {
        return ppmRead(path, out, errbuf, errbufCap);
    }
    if (fmt == FMT_PNG) {
        return pngRead(path, out, errbuf, errbufCap);
    }
    if (errbuf != NULL && errbufCap > 0) {
        snprintf(errbuf, errbufCap, "unrecognized image format (not PPM or PNG)");
    }
    return false;
}

static void usage(void) {
    fprintf(stderr, "usage: imgdiff compare [--max-channel-delta N] [--max-diff-pixels N]\n"
                    "                       [--mask FILE] [--diff-out OUT.png] ACTUAL REF\n"
                    "       imgdiff convert IN OUT\n");
}

static int cmdCompare(int argc, char **argv) {
    int maxChannelDelta = 0;
    long maxDiffPixels = 0;
    const char *maskPath = NULL;
    const char *diffOutPath = NULL;
    const char *actualPath = NULL;
    const char *refPath = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--max-channel-delta") == 0 && i + 1 < argc) {
            maxChannelDelta = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-diff-pixels") == 0 && i + 1 < argc) {
            maxDiffPixels = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--mask") == 0 && i + 1 < argc) {
            maskPath = argv[++i];
        } else if (strcmp(argv[i], "--diff-out") == 0 && i + 1 < argc) {
            diffOutPath = argv[++i];
        } else if (actualPath == NULL) {
            actualPath = argv[i];
        } else if (refPath == NULL) {
            refPath = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (actualPath == NULL || refPath == NULL) {
        usage();
        return 2;
    }

    char err[256];
    Image actual, ref;
    if (!loadImage(actualPath, &actual, err, sizeof(err))) {
        fprintf(stderr, "imgdiff: %s: %s\n", actualPath, err);
        return 2;
    }
    if (!loadImage(refPath, &ref, err, sizeof(err))) {
        fprintf(stderr, "imgdiff: %s: %s\n", refPath, err);
        imageFree(&actual);
        return 2;
    }

    MaskList mask = {0};
    if (maskPath != NULL &&
        !maskLoad(maskPath, actual.width, actual.height, &mask, err, sizeof(err))) {
        fprintf(stderr, "imgdiff: %s: %s\n", maskPath, err);
        imageFree(&actual);
        imageFree(&ref);
        return 2;
    }

    CompareResult result;
    bool matched =
        imagesCompare(&actual, &ref, maxChannelDelta, (uint32_t)maxDiffPixels, &mask, &result);

    int rc;
    if (!result.dimensionsMatch) {
        fprintf(stderr, "imgdiff: dimension mismatch: %ux%u vs %ux%u\n", actual.width,
                actual.height, ref.width, ref.height);
        rc = 1;
    } else if (!matched) {
        fprintf(stderr, "imgdiff: %u pixels differ (max delta %u), first at (%u,%u)\n",
                result.diffPixelCount, result.maxDeltaSeen, result.firstDiffX, result.firstDiffY);
        rc = 1;
    } else {
        rc = 0;
    }

    if (rc == 1 && diffOutPath != NULL && result.dimensionsMatch) {
        if (!writeDiffImage(diffOutPath, &actual, &ref, &mask, maxChannelDelta, err, sizeof(err))) {
            fprintf(stderr, "imgdiff: %s: %s\n", diffOutPath, err);
        }
    }

    maskFree(&mask);
    imageFree(&actual);
    imageFree(&ref);
    return rc;
}

static int cmdConvert(int argc, char **argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const char *inPath = argv[0];
    const char *outPath = argv[1];

    char err[256];
    Image img;
    if (!loadImage(inPath, &img, err, sizeof(err))) {
        fprintf(stderr, "imgdiff: %s: %s\n", inPath, err);
        return 2;
    }
    Format outFmt = formatFromExtension(outPath);
    bool ok;
    if (outFmt == FMT_PNG) {
        ok = pngWrite(outPath, &img, err, sizeof(err));
    } else if (outFmt == FMT_PPM) {
        ok = ppmWrite(outPath, &img, err, sizeof(err));
    } else {
        fprintf(stderr, "imgdiff: %s: unrecognized output extension (.png or .ppm)\n", outPath);
        imageFree(&img);
        return 2;
    }
    imageFree(&img);
    if (!ok) {
        fprintf(stderr, "imgdiff: %s: %s\n", outPath, err);
        return 2;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "compare") == 0) {
        return cmdCompare(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "convert") == 0) {
        return cmdConvert(argc - 2, argv + 2);
    }
    usage();
    return 2;
}
