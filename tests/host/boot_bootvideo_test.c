/* Host tests for boot/common/bootvideo.c: the shared GOP/VBE mode-accept and pick rule
 * (ARCHITECTURE §5.5, D-068/D-109). */
#include "framework/test.h"
#include "bootvideo.h"

static BootVideoMode mkMode(uint32_t id, uint32_t w, uint32_t h, uint32_t r, uint32_t g, uint32_t b,
                            uint32_t resv) {
    BootVideoMode m = {0};
    m.id = id;
    m.width = w;
    m.height = h;
    m.pitch = w * 4u;
    m.redMask = r;
    m.greenMask = g;
    m.blueMask = b;
    m.reservedMask = resv;
    m.fbPhys = 0x1000ULL + id;
    return m;
}

TEST(bootVideoAcceptRgbxBgrx) {
    BootVideoMode rgbx = mkMode(0, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    ASSERT_TRUE(bootVideoAccept(&rgbx));
    BootVideoMode bgrx = mkMode(1, 1024, 768, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
    ASSERT_TRUE(bootVideoAccept(&bgrx));
}

TEST(bootVideoAcceptRejectsZeroOrNoncontiguousMask) {
    BootVideoMode zeroBlue = mkMode(0, 1024, 768, 0xFF, 0xFF00, 0, 0xFF000000);
    ASSERT_TRUE(!bootVideoAccept(&zeroBlue));
    BootVideoMode gap = mkMode(0, 1024, 768, 0xF0F, 0xFF00, 0xFF0000, 0xFF000000);
    ASSERT_TRUE(!bootVideoAccept(&gap));
}

TEST(bootVideoAcceptRejectsWiderThan8BitChannel) {
    /* A 10-bit-per-channel mode (2:10:10:10): each channel is contiguous but 10 bits wide. */
    BootVideoMode m = mkMode(0, 1024, 768, 0x3FF, 0xFFC00, 0x3FF00000, 0xC0000000);
    ASSERT_TRUE(!bootVideoAccept(&m));
}

TEST(bootVideoAcceptRejectsShortPitch) {
    BootVideoMode m = mkMode(0, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    m.pitch = 1024 * 3; /* less than width*4 */
    ASSERT_TRUE(!bootVideoAccept(&m));
}

TEST(bootVideoAcceptRejectsTooNarrowHighBit) {
    /* Highest set bit at 15 (16-bit-ish field), not in 24..31. */
    BootVideoMode m = mkMode(0, 1024, 768, 0x1F, 0x1FE0, 0x1F0000, 0);
    ASSERT_TRUE(!bootVideoAccept(&m));
}

TEST(bootVideoPickerAutoLargestArea) {
    BootVideoPicker p;
    bootVideoPickerInit(&p, 0, 0);
    BootVideoMode small = mkMode(0, 800, 600, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    BootVideoMode big = mkMode(1, 1920, 1080, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    bootVideoPickerOffer(&p, &small);
    bootVideoPickerOffer(&p, &big);
    BootVideoMode out;
    bool fellBack = true;
    ASSERT_TRUE(bootVideoPickerResult(&p, &out, &fellBack));
    ASSERT_EQ(out.id, 1u);
    ASSERT_TRUE(!fellBack);
}

TEST(bootVideoPickerAutoRejectsOverMaxDim) {
    BootVideoPicker p;
    bootVideoPickerInit(&p, 0, 0);
    BootVideoMode ok = mkMode(0, 1920, 1080, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    BootVideoMode tooBig = mkMode(1, 4096, 2160, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    bootVideoPickerOffer(&p, &ok);
    bootVideoPickerOffer(&p, &tooBig);
    BootVideoMode out;
    bool fellBack = false;
    ASSERT_TRUE(bootVideoPickerResult(&p, &out, &fellBack));
    ASSERT_EQ(out.id, 0u);
}

TEST(bootVideoPickerAutoTiesByWidthThenLowerId) {
    BootVideoPicker p;
    bootVideoPickerInit(&p, 0, 0);
    /* Same area (1024x768 vs 768x1024 both 786432), different width: wider wins. */
    BootVideoMode wide = mkMode(5, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    BootVideoMode tall = mkMode(3, 768, 1024, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    bootVideoPickerOffer(&p, &tall);
    bootVideoPickerOffer(&p, &wide);
    BootVideoMode out;
    bool fellBack = false;
    ASSERT_TRUE(bootVideoPickerResult(&p, &out, &fellBack));
    ASSERT_EQ(out.id, 5u);

    /* Same area and width, different id, offered out of order: lower id wins regardless of
     * offer order (D-109: "compared explicitly, so enumeration order doesn't matter"). */
    BootVideoPicker p2;
    bootVideoPickerInit(&p2, 0, 0);
    BootVideoMode idHigh = mkMode(7, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    BootVideoMode idLow = mkMode(2, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    bootVideoPickerOffer(&p2, &idHigh);
    bootVideoPickerOffer(&p2, &idLow);
    BootVideoMode out2;
    ASSERT_TRUE(bootVideoPickerResult(&p2, &out2, &fellBack));
    ASSERT_EQ(out2.id, 2u);
}

TEST(bootVideoPickerExactMatchLowestId) {
    BootVideoPicker p;
    bootVideoPickerInit(&p, 1024, 768);
    BootVideoMode a = mkMode(4, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    BootVideoMode b = mkMode(1, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    BootVideoMode other = mkMode(0, 1920, 1080, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    bootVideoPickerOffer(&p, &other);
    bootVideoPickerOffer(&p, &a);
    bootVideoPickerOffer(&p, &b);
    BootVideoMode out;
    bool fellBack = true;
    ASSERT_TRUE(bootVideoPickerResult(&p, &out, &fellBack));
    ASSERT_EQ(out.id, 1u);
    ASSERT_TRUE(!fellBack);
}

TEST(bootVideoPickerExactMissingFallsBackToAuto) {
    BootVideoPicker p;
    bootVideoPickerInit(&p, 1280, 720);
    BootVideoMode only = mkMode(0, 1920, 1080, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    bootVideoPickerOffer(&p, &only);
    BootVideoMode out;
    bool fellBack = false;
    ASSERT_TRUE(bootVideoPickerResult(&p, &out, &fellBack));
    ASSERT_EQ(out.id, 0u);
    ASSERT_TRUE(fellBack);
}

TEST(bootVideoPickerResultFalseWhenNothingOffered) {
    BootVideoPicker p;
    bootVideoPickerInit(&p, 0, 0);
    BootVideoMode out;
    bool fellBack = false;
    ASSERT_TRUE(!bootVideoPickerResult(&p, &out, &fellBack));
}

TEST(bootVideoToFramebufferComputesShiftsAndSizes) {
    BootVideoMode m = mkMode(0, 1024, 768, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
    m.fbPhys = 0xDEAD0000ULL;
    BootFramebuffer fb;
    bootVideoToFramebuffer(&m, &fb);
    ASSERT_EQ(fb.phys, 0xDEAD0000ULL);
    ASSERT_EQ(fb.width, 1024u);
    ASSERT_EQ(fb.height, 768u);
    ASSERT_EQ(fb.pitch, 1024u * 4u);
    ASSERT_EQ((uint32_t)fb.bpp, 32u);
    ASSERT_EQ((uint32_t)fb.redShift, 0u);
    ASSERT_EQ((uint32_t)fb.redSize, 8u);
    ASSERT_EQ((uint32_t)fb.greenShift, 8u);
    ASSERT_EQ((uint32_t)fb.greenSize, 8u);
    ASSERT_EQ((uint32_t)fb.blueShift, 16u);
    ASSERT_EQ((uint32_t)fb.blueSize, 8u);
}
