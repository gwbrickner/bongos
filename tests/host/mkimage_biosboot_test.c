/* Host tests for tools/mkimage/biosboot.c: the stage1 patch-block writer and stage2 header
 * validator (D-099). */
#include "biosboot.h"
#include "framework/test.h"

#include <stdlib.h>
#include <string.h>

static void makeStage1(uint8_t stage1[BIOSBOOT_STAGE1_SIZE], int withMagic) {
    memset(stage1, 0xCC, BIOSBOOT_STAGE1_SIZE);
    if (withMagic) {
        uint32_t magic = BIOSBOOT_S1PB_MAGIC;
        memcpy(stage1 + BIOSBOOT_PATCH_OFFSET, &magic, sizeof(magic));
    }
}

TEST(mkimagePatchStage1WritesLbaAndSectors) {
    uint8_t stage1[BIOSBOOT_STAGE1_SIZE];
    makeStage1(stage1, 1);
    ASSERT_EQ(mkimagePatchStage1(stage1, 2048, 100), 0);

    uint64_t lba;
    uint16_t sectors, reserved;
    memcpy(&lba, stage1 + BIOSBOOT_PATCH_OFFSET + 4, sizeof(lba));
    memcpy(&sectors, stage1 + BIOSBOOT_PATCH_OFFSET + 12, sizeof(sectors));
    memcpy(&reserved, stage1 + BIOSBOOT_PATCH_OFFSET + 14, sizeof(reserved));
    ASSERT_EQ(lba, 2048ULL);
    ASSERT_EQ(sectors, 100u);
    ASSERT_EQ(reserved, 0u);
}

TEST(mkimagePatchStage1RejectsMissingMagic) {
    uint8_t stage1[BIOSBOOT_STAGE1_SIZE];
    makeStage1(stage1, 0);
    ASSERT_EQ(mkimagePatchStage1(stage1, 2048, 100), -1);
}

TEST(mkimagePatchStage1RejectsZeroOrTooManySectors) {
    uint8_t stage1[BIOSBOOT_STAGE1_SIZE];
    makeStage1(stage1, 1);
    ASSERT_EQ(mkimagePatchStage1(stage1, 2048, 0), -1);
    makeStage1(stage1, 1);
    ASSERT_EQ(mkimagePatchStage1(stage1, 2048, BIOSBOOT_STAGE2_MAX_SECTORS + 1), -1);
    makeStage1(stage1, 1);
    ASSERT_EQ(mkimagePatchStage1(stage1, 2048, BIOSBOOT_STAGE2_MAX_SECTORS), 0);
}

static void makeStage2Header(uint8_t *buf, uint32_t magic, uint32_t fileSize, uint32_t version) {
    memcpy(buf + 4, &magic, 4);
    memcpy(buf + 8, &fileSize, 4);
    memcpy(buf + 12, &version, 4);
}

TEST(mkimageValidateStage2AcceptsMatchingHeader) {
    uint8_t buf[600];
    memset(buf, 0, sizeof(buf));
    makeStage2Header(buf, BIOSBOOT_S2HD_MAGIC, sizeof(buf), BIOSBOOT_S2HD_VERSION);
    uint32_t sectors = mkimageValidateStage2(buf, sizeof(buf));
    ASSERT_EQ(sectors, (uint32_t)((sizeof(buf) + BIOSBOOT_SECTOR_SIZE - 1) / BIOSBOOT_SECTOR_SIZE));
}

TEST(mkimageValidateStage2RejectsBadMagicOrVersion) {
    uint8_t buf[600];
    memset(buf, 0, sizeof(buf));
    makeStage2Header(buf, 0xDEADBEEFu, sizeof(buf), BIOSBOOT_S2HD_VERSION);
    ASSERT_EQ(mkimageValidateStage2(buf, sizeof(buf)), 0u);

    makeStage2Header(buf, BIOSBOOT_S2HD_MAGIC, sizeof(buf), 2 /* wrong version */);
    ASSERT_EQ(mkimageValidateStage2(buf, sizeof(buf)), 0u);
}

TEST(mkimageValidateStage2RejectsFileSizeMismatch) {
    uint8_t buf[600];
    memset(buf, 0, sizeof(buf));
    makeStage2Header(buf, BIOSBOOT_S2HD_MAGIC, sizeof(buf) - 1 /* wrong */, BIOSBOOT_S2HD_VERSION);
    ASSERT_EQ(mkimageValidateStage2(buf, sizeof(buf)), 0u);
}

TEST(mkimageValidateStage2RejectsOversizedImage) {
    size_t tooBig = (size_t)BIOSBOOT_STAGE2_MAX_SECTORS * BIOSBOOT_SECTOR_SIZE + 1;
    uint8_t *buf = malloc(tooBig);
    ASSERT_TRUE(buf != NULL);
    memset(buf, 0, tooBig);
    makeStage2Header(buf, BIOSBOOT_S2HD_MAGIC, (uint32_t)tooBig, BIOSBOOT_S2HD_VERSION);
    ASSERT_EQ(mkimageValidateStage2(buf, tooBig), 0u);
    free(buf);
}
