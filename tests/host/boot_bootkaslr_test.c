/* Host tests for boot/common/bootkaslr.c (M2.6, D-121): slide selection. */
#include "bootkaslr.h"
#include "bootmem.h"
#include "framework/test.h"

#include <string.h>

#define TWO_MIB 0x200000ULL

static void fillSeed(uint8_t seed[64], uint64_t n) {
    memset(seed, 0x5A, 64);
    memcpy(seed, &n, 8);
    uint64_t m = n * 0x9E3779B97F4A7C15ULL;
    memcpy(seed + 40, &m, 8);
}

TEST(kaslrSlideIsAlignedAndInsideTheWindow) {
    uint8_t seed[64];
    static const uint64_t spans[] = {1,          0x1000,     0x200000,   0x200001,  0x800000,
                                     0x1F800000, 0x1FE00000, 0x1FE00001, 0x20000000};
    for (uint32_t s = 0; s < sizeof(spans) / sizeof(spans[0]); s++) {
        uint64_t need = bootAlignUp(spans[s], TWO_MIB);
        for (uint64_t n = 0; n < 500; n++) {
            fillSeed(seed, n);
            uint64_t slide = 0xDEAD;
            ASSERT_EQ(bootKaslrPickSlide(seed, spans[s], &slide), BOOT_OK);
            ASSERT_EQ(slide & (TWO_MIB - 1), 0ULL);
            ASSERT_TRUE(slide + need <= BOOT_KASLR_WINDOW);
        }
    }
}

TEST(kaslrIsDeterministic) {
    uint8_t seed[64];
    fillSeed(seed, 77);
    uint64_t a = 1, b = 2;
    ASSERT_EQ(bootKaslrPickSlide(seed, 0x800000, &a), BOOT_OK);
    ASSERT_EQ(bootKaslrPickSlide(seed, 0x800000, &b), BOOT_OK);
    ASSERT_EQ(a, b);
}

TEST(kaslrKnownAnswers) {
    /* Computed with an independent Python implementation of the design in D-121. */
    uint8_t zero[64];
    memset(zero, 0, sizeof(zero));
    uint8_t ff[64];
    memset(ff, 0xFF, sizeof(ff));
    uint8_t ramp[64];
    for (uint32_t i = 0; i < 64; i++) {
        ramp[i] = (uint8_t)i;
    }
    uint64_t slide;
    ASSERT_EQ(bootKaslrPickSlide(zero, 0x1000, &slide), BOOT_OK);
    ASSERT_EQ(slide, 0x0EC00000ULL);
    ASSERT_EQ(bootKaslrPickSlide(ff, 0x1000, &slide), BOOT_OK);
    ASSERT_EQ(slide, 0x12600000ULL);
    ASSERT_EQ(bootKaslrPickSlide(ramp, 0x1000, &slide), BOOT_OK);
    ASSERT_EQ(slide, 0x13E00000ULL);
    ASSERT_EQ(bootKaslrPickSlide(ramp, 0x800000, &slide), BOOT_OK);
    ASSERT_EQ(slide, 0x13A00000ULL);
}

TEST(kaslrEverySeedByteMattersToTheHash) {
    /* Flipping one byte of any of the 8 qwords must be able to change the slide: over many
     * different flip values at each byte position, at least 2 distinct slides appear. */
    uint8_t base[64];
    fillSeed(base, 1);
    for (uint32_t pos = 0; pos < 64; pos++) {
        uint64_t first = 0;
        int differs = 0;
        for (uint32_t v = 0; v < 64; v++) {
            uint8_t seed[64];
            memcpy(seed, base, 64);
            seed[pos] = (uint8_t)(v * 4 + 1);
            uint64_t slide;
            ASSERT_EQ(bootKaslrPickSlide(seed, 0x800000, &slide), BOOT_OK);
            if (v == 0) {
                first = slide;
            } else if (slide != first) {
                differs = 1;
            }
        }
        ASSERT_TRUE(differs);
    }
}

TEST(kaslrCoversManySlotsAndBothEnds) {
    /* span 8 MiB -> 253 slots. 8192 distinct seeds must reach nearly all of them, including slot
     * 0 and the last slot, and never anything outside. */
    const uint64_t span = 0x800000;
    const uint64_t slots = ((BOOT_KASLR_WINDOW - span) >> 21) + 1;
    ASSERT_EQ(slots, 253ULL);
    static uint32_t hits[256];
    memset(hits, 0, sizeof(hits));
    uint8_t seed[64];
    for (uint64_t n = 0; n < 8192; n++) {
        fillSeed(seed, n);
        uint64_t slide;
        ASSERT_EQ(bootKaslrPickSlide(seed, span, &slide), BOOT_OK);
        uint64_t slot = slide >> 21;
        ASSERT_TRUE(slot < slots);
        hits[slot]++;
    }
    uint32_t distinct = 0;
    for (uint64_t i = 0; i < slots; i++) {
        if (hits[i] != 0) {
            distinct++;
        }
        /* Expected 32.4 per slot; a broken (e.g. constant or low-entropy) mapping fails this. */
        ASSERT_TRUE(hits[i] < 100);
    }
    ASSERT_TRUE(distinct >= 250);
    ASSERT_TRUE(hits[0] > 0);
    ASSERT_TRUE(hits[slots - 1] > 0);
}

TEST(kaslrWindowSizedSpanHasOnlySlotZero) {
    uint8_t seed[64];
    for (uint64_t n = 0; n < 100; n++) {
        fillSeed(seed, n);
        uint64_t slide = 1;
        ASSERT_EQ(bootKaslrPickSlide(seed, BOOT_KASLR_WINDOW, &slide), BOOT_OK);
        ASSERT_EQ(slide, 0ULL);
        ASSERT_EQ(bootKaslrPickSlide(seed, BOOT_KASLR_WINDOW - TWO_MIB + 1, &slide), BOOT_OK);
        ASSERT_EQ(slide, 0ULL);
    }
}

TEST(kaslrTwoSlotsWhenOneAlignedUnitOfRoom) {
    uint8_t seed[64];
    int seen[2] = {0, 0};
    for (uint64_t n = 0; n < 200; n++) {
        fillSeed(seed, n);
        uint64_t slide;
        ASSERT_EQ(bootKaslrPickSlide(seed, BOOT_KASLR_WINDOW - TWO_MIB, &slide), BOOT_OK);
        ASSERT_TRUE(slide == 0 || slide == TWO_MIB);
        seen[slide >> 21] = 1;
    }
    ASSERT_TRUE(seen[0] && seen[1]);
}

TEST(kaslrRejectsZeroAndOversizedSpan) {
    uint8_t seed[64];
    fillSeed(seed, 3);
    uint64_t slide = 0x1234;
    ASSERT_EQ(bootKaslrPickSlide(seed, 0, &slide), BOOT_ERR_ELF_RANGE);
    ASSERT_EQ(bootKaslrPickSlide(seed, BOOT_KASLR_WINDOW + 1, &slide), BOOT_ERR_ELF_RANGE);
    ASSERT_EQ(bootKaslrPickSlide(seed, 0xFFFFFFFFFFFFFFFFULL, &slide), BOOT_ERR_ELF_RANGE);
    ASSERT_EQ(bootKaslrPickSlide(seed, 0xFFFFFFFFFFE00001ULL, &slide), BOOT_ERR_ELF_RANGE);
    ASSERT_EQ(slide, 0x1234ULL); /* never written on failure */
}

TEST(kaslrRejectsNullArguments) {
    uint8_t seed[64];
    fillSeed(seed, 3);
    uint64_t slide;
    ASSERT_EQ(bootKaslrPickSlide(NULL, 0x1000, &slide), BOOT_ERR_ELF_HEADER);
    ASSERT_EQ(bootKaslrPickSlide(seed, 0x1000, NULL), BOOT_ERR_ELF_HEADER);
}
