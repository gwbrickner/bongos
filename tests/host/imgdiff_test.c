/* Host tests for tools/imgdiff's PPM/PNG/DEFLATE codec (ARCHITECTURE §23, D-070). */
/* -std=c17 alone hides POSIX declarations (truncate, unlink); this is host tooling, not the
 * freestanding kernel/loader C17 subset ARCHITECTURE §4 restricts (matches tools/mkimage/main.c's
 * own reasoning for the same #define). */
#define _DEFAULT_SOURCE
#include "compare.h"
#include "framework/test.h"
#include "image.h"
#include "png.h"
#include "ppm.h"
#include "zlib_wrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A fresh temp file path each call (process-unique prefix + an incrementing counter -- host
 * tests run single-threaded and sequentially, so no lock is needed). Never collides with a real
 * file the test suite cares about. */
static void tmpPath(char *buf, size_t cap) {
    static int counter = 0;
    snprintf(buf, cap, "/tmp/bongos-imgdiff-test-%d-%d.tmp", (int)getpid(), counter++);
}

static void fillSolid(Image *img, uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t i = 0; i < img->width * img->height; i++) {
        img->rgb[i * 3 + 0] = r;
        img->rgb[i * 3 + 1] = g;
        img->rgb[i * 3 + 2] = b;
    }
}

static void fillGradient(Image *img) {
    for (uint32_t y = 0; y < img->height; y++) {
        for (uint32_t x = 0; x < img->width; x++) {
            size_t i = ((size_t)y * img->width + x) * 3;
            img->rgb[i + 0] = (uint8_t)((x * 255) / (img->width > 1 ? img->width - 1 : 1));
            img->rgb[i + 1] = (uint8_t)((y * 255) / (img->height > 1 ? img->height - 1 : 1));
            img->rgb[i + 2] = 128;
        }
    }
}

/* Deterministic pseudo-noise (not a real PRNG -- just a reproducible, non-uniform byte pattern,
 * so a compressor sees genuinely high-entropy-looking input without this test depending on
 * rand()'s implementation-defined sequence). */
static void fillNoise(Image *img) {
    for (uint32_t y = 0; y < img->height; y++) {
        for (uint32_t x = 0; x < img->width; x++) {
            size_t i = ((size_t)y * img->width + x) * 3;
            img->rgb[i + 0] = (uint8_t)(x * 167u + y * 113u + 29u);
            img->rgb[i + 1] = (uint8_t)(x * 71u + y * 197u + 61u);
            img->rgb[i + 2] = (uint8_t)(x * 233u + y * 7u + 5u);
        }
    }
}

static void pngRoundTrip(Image *src) {
    char path[128];
    tmpPath(path, sizeof(path));
    char err[256];
    ASSERT_TRUE(pngWrite(path, src, err, sizeof(err)));

    Image loaded;
    ASSERT_TRUE(pngRead(path, &loaded, err, sizeof(err)));
    ASSERT_EQ(loaded.width, src->width);
    ASSERT_EQ(loaded.height, src->height);
    ASSERT_EQ(memcmp(loaded.rgb, src->rgb, (size_t)src->width * src->height * 3), 0);
    imageFree(&loaded);
    unlink(path);
}

TEST(imgdiffPngRoundTripFlat) {
    Image img;
    ASSERT_TRUE(imageAlloc(&img, 17, 13));
    fillSolid(&img, 200, 100, 50);
    pngRoundTrip(&img);
    imageFree(&img);
}

TEST(imgdiffPngRoundTripGradient) {
    Image img;
    ASSERT_TRUE(imageAlloc(&img, 64, 48));
    fillGradient(&img);
    pngRoundTrip(&img);
    imageFree(&img);
}

TEST(imgdiffPngRoundTripNoise) {
    Image img;
    ASSERT_TRUE(imageAlloc(&img, 37, 29));
    fillNoise(&img);
    pngRoundTrip(&img);
    imageFree(&img);
}

TEST(imgdiffPpmRoundTrip) {
    Image img;
    ASSERT_TRUE(imageAlloc(&img, 12, 9));
    fillGradient(&img);

    char path[128];
    tmpPath(path, sizeof(path));
    char err[256];
    ASSERT_TRUE(ppmWrite(path, &img, err, sizeof(err)));

    Image loaded;
    ASSERT_TRUE(ppmRead(path, &loaded, err, sizeof(err)));
    ASSERT_EQ(loaded.width, img.width);
    ASSERT_EQ(loaded.height, img.height);
    ASSERT_EQ(memcmp(loaded.rgb, img.rgb, (size_t)img.width * img.height * 3), 0);

    imageFree(&loaded);
    imageFree(&img);
    unlink(path);
}

TEST(imgdiffPngRejectsBadCrc) {
    Image img;
    ASSERT_TRUE(imageAlloc(&img, 8, 8));
    fillSolid(&img, 10, 20, 30);
    char path[128];
    tmpPath(path, sizeof(path));
    char err[256];
    ASSERT_TRUE(pngWrite(path, &img, err, sizeof(err)));
    imageFree(&img);

    FILE *f = fopen(path, "r+b");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(fseek(f, 20, SEEK_SET), 0); /* well inside the IHDR chunk's data/CRC */
    int c = fgetc(f);
    ASSERT_TRUE(c != EOF);
    ASSERT_EQ(fseek(f, 20, SEEK_SET), 0);
    ASSERT_EQ(fputc(c ^ 0xFF, f), c ^ 0xFF);
    fclose(f);

    Image loaded;
    ASSERT_TRUE(!pngRead(path, &loaded, err, sizeof(err)));
    unlink(path);
}

TEST(imgdiffPngRejectsTruncated) {
    Image img;
    ASSERT_TRUE(imageAlloc(&img, 8, 8));
    fillGradient(&img);
    char path[128];
    tmpPath(path, sizeof(path));
    char err[256];
    ASSERT_TRUE(pngWrite(path, &img, err, sizeof(err)));
    imageFree(&img);

    /* Truncate to just the signature + a partial IHDR. */
    ASSERT_EQ(truncate(path, 20), 0);
    Image loaded;
    ASSERT_TRUE(!pngRead(path, &loaded, err, sizeof(err)));
    unlink(path);
}

TEST(imgdiffZlibRejectsBadAdler) {
    uint8_t data[64];
    for (int i = 0; i < 64; i++) {
        data[i] = (uint8_t)(i * 3);
    }
    uint8_t zbuf[256];
    size_t zlen = 0;
    ASSERT_TRUE(zlibDeflate(data, sizeof(data), 0, zbuf, sizeof(zbuf), &zlen));
    zbuf[zlen - 1] ^= 0xFF; /* corrupt the last Adler-32 byte */

    uint8_t out[64];
    size_t outLen = 0;
    ASSERT_TRUE(!zlibInflate(zbuf, zlen, out, sizeof(out), &outLen));
}

TEST(imgdiffZlibRoundTrip) {
    uint8_t data[300];
    for (int i = 0; i < 300; i++) {
        data[i] = (uint8_t)((i * 37) ^ (i >> 3));
    }
    uint8_t zbuf[512];
    size_t zlen = 0;
    ASSERT_TRUE(zlibDeflate(data, sizeof(data), 0, zbuf, sizeof(zbuf), &zlen));

    uint8_t out[300];
    size_t outLen = 0;
    ASSERT_TRUE(zlibInflate(zbuf, zlen, out, sizeof(out), &outLen));
    ASSERT_EQ(outLen, (size_t)300);
    ASSERT_EQ(memcmp(out, data, sizeof(data)), 0);
}

/* Decodes fixtures written by tests/data/png/gen.py using Python's stdlib zlib -- an independent
 * encoder, so this cross-checks inflate.c/png.c's defiltering against something other than our
 * own encoder. Run from the repo root (as `make host-tests` always is), matching every other
 * relative path this build assumes. */
TEST(imgdiffDecodesIndependentFixtures) {
    Image img;
    char err[256];

    ASSERT_TRUE(pngRead("tests/data/png/filter0_solid.png", &img, err, sizeof(err)));
    ASSERT_EQ(img.width, (uint32_t)8);
    ASSERT_EQ(img.height, (uint32_t)8);
    ASSERT_EQ(img.rgb[0], 200);
    ASSERT_EQ(img.rgb[1], 100);
    ASSERT_EQ(img.rgb[2], 50);
    ASSERT_EQ(img.rgb[(63) * 3 + 0], 200); /* every pixel is the same solid color */
    imageFree(&img);

    static const char *filterFixtures[] = {
        "tests/data/png/filter1_sub.png",
        "tests/data/png/filter2_up.png",
        "tests/data/png/filter3_avg.png",
        "tests/data/png/filter4_paeth.png",
    };
    for (size_t i = 0; i < sizeof(filterFixtures) / sizeof(filterFixtures[0]); i++) {
        Image g;
        ASSERT_TRUE(pngRead(filterFixtures[i], &g, err, sizeof(err)));
        ASSERT_EQ(g.width, (uint32_t)8);
        ASSERT_EQ(g.height, (uint32_t)8);
        /* Every fixture encodes the same gradient formula (gen.py's gradient_rows), regardless of
         * which filter type compressed it -- correct defiltering must recover it exactly. */
        for (uint32_t y = 0; y < g.height; y++) {
            for (uint32_t x = 0; x < g.width; x++) {
                size_t idx = ((size_t)y * g.width + x) * 3;
                ASSERT_EQ(g.rgb[idx + 0], (uint8_t)((x * 255) / 7));
                ASSERT_EQ(g.rgb[idx + 1], (uint8_t)((y * 255) / 7));
                ASSERT_EQ(g.rgb[idx + 2], 128);
            }
        }
        imageFree(&g);
    }

    Image mixed;
    ASSERT_TRUE(pngRead("tests/data/png/mixed_filters.png", &mixed, err, sizeof(err)));
    ASSERT_EQ(mixed.width, (uint32_t)16);
    ASSERT_EQ(mixed.height, (uint32_t)16);
    for (uint32_t y = 0; y < mixed.height; y++) {
        for (uint32_t x = 0; x < mixed.width; x++) {
            size_t idx = ((size_t)y * mixed.width + x) * 3;
            ASSERT_EQ(mixed.rgb[idx + 0], (uint8_t)((x * 255) / 15));
            ASSERT_EQ(mixed.rgb[idx + 1], (uint8_t)((y * 255) / 15));
            ASSERT_EQ(mixed.rgb[idx + 2], 128);
        }
    }
    imageFree(&mixed);
}

TEST(imgdiffCompareExactMatch) {
    Image a, b;
    ASSERT_TRUE(imageAlloc(&a, 10, 10));
    ASSERT_TRUE(imageAlloc(&b, 10, 10));
    fillGradient(&a);
    fillGradient(&b);
    CompareResult result;
    ASSERT_TRUE(imagesCompare(&a, &b, 0, 0, NULL, &result));
    ASSERT_EQ(result.diffPixelCount, (uint32_t)0);
    imageFree(&a);
    imageFree(&b);
}

TEST(imgdiffCompareDetectsDifference) {
    Image a, b;
    ASSERT_TRUE(imageAlloc(&a, 4, 4));
    ASSERT_TRUE(imageAlloc(&b, 4, 4));
    fillSolid(&a, 0, 0, 0);
    fillSolid(&b, 0, 0, 0);
    b.rgb[(2 * 4 + 1) * 3 + 0] = 255; /* pixel (1,2) differs */

    CompareResult result;
    ASSERT_TRUE(!imagesCompare(&a, &b, 0, 0, NULL, &result));
    ASSERT_EQ(result.diffPixelCount, (uint32_t)1);
    ASSERT_EQ(result.firstDiffX, (uint32_t)1);
    ASSERT_EQ(result.firstDiffY, (uint32_t)2);
    ASSERT_EQ(result.maxDeltaSeen, (uint32_t)255);

    /* Under a tolerance that covers the delta, or a diff-pixel budget of 1, it's a match. */
    ASSERT_TRUE(imagesCompare(&a, &b, 255, 0, NULL, &result));
    ASSERT_TRUE(imagesCompare(&a, &b, 0, 1, NULL, &result));

    imageFree(&a);
    imageFree(&b);
}

TEST(imgdiffCompareMaskExcludesDifference) {
    Image a, b;
    ASSERT_TRUE(imageAlloc(&a, 4, 4));
    ASSERT_TRUE(imageAlloc(&b, 4, 4));
    fillSolid(&a, 0, 0, 0);
    fillSolid(&b, 0, 0, 0);
    b.rgb[(2 * 4 + 1) * 3 + 0] = 255; /* pixel (1,2) differs */

    MaskRect rect = {0, 2, 4, 1}; /* covers row y=2 entirely */
    MaskList mask = {&rect, 1};
    CompareResult result;
    ASSERT_TRUE(imagesCompare(&a, &b, 0, 0, &mask, &result));
    ASSERT_EQ(result.diffPixelCount, (uint32_t)0);

    imageFree(&a);
    imageFree(&b);
}

TEST(imgdiffMaskLoadParsesAndClips) {
    char path[128];
    tmpPath(path, sizeof(path));
    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    fprintf(f, "# a comment\n\n2 2 5 5\n0 0 100 100\n");
    fclose(f);

    MaskList mask;
    char err[256];
    ASSERT_TRUE(maskLoad(path, 10, 10, &mask, err, sizeof(err)));
    ASSERT_EQ(mask.count, (uint32_t)2);
    ASSERT_EQ(mask.rects[0].x, (uint32_t)2);
    ASSERT_EQ(mask.rects[0].w, (uint32_t)5);
    /* "0 0 100 100" clips to the 10x10 image. */
    ASSERT_EQ(mask.rects[1].w, (uint32_t)10);
    ASSERT_EQ(mask.rects[1].h, (uint32_t)10);

    maskFree(&mask);
    unlink(path);
}
