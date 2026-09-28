/* See biosboot.h. Field offsets are docs/specs/bios-boot.md's patch-block/stage2-header layout
 * (D-099). Host tooling, native byte order (ARCHITECTURE §1.1 requires a little-endian build
 * host), so fields are written/read directly rather than byteswapped. */
#include "biosboot.h"

#include <string.h>

int mkimagePatchStage1(uint8_t stage1[BIOSBOOT_STAGE1_SIZE], uint64_t stage2Lba,
                       uint32_t stage2Sectors) {
    uint32_t magic;
    memcpy(&magic, stage1 + BIOSBOOT_PATCH_OFFSET, sizeof(magic));
    if (magic != BIOSBOOT_S1PB_MAGIC) {
        return -1;
    }
    if (stage2Sectors == 0 || stage2Sectors > BIOSBOOT_STAGE2_MAX_SECTORS) {
        return -1;
    }
    memcpy(stage1 + BIOSBOOT_PATCH_OFFSET + 4, &stage2Lba, sizeof(stage2Lba));
    uint16_t sectors16 = (uint16_t)stage2Sectors;
    memcpy(stage1 + BIOSBOOT_PATCH_OFFSET + 12, &sectors16, sizeof(sectors16));
    uint16_t zero = 0;
    memcpy(stage1 + BIOSBOOT_PATCH_OFFSET + 14, &zero, sizeof(zero));
    return 0;
}

uint32_t mkimageValidateStage2(const uint8_t *stage2, size_t stage2Len) {
    if (stage2Len < 16 ||
        stage2Len > (uint64_t)BIOSBOOT_STAGE2_MAX_SECTORS * BIOSBOOT_SECTOR_SIZE) {
        return 0;
    }
    uint32_t magic, fileSize, version;
    memcpy(&magic, stage2 + 4, sizeof(magic));
    memcpy(&fileSize, stage2 + 8, sizeof(fileSize));
    memcpy(&version, stage2 + 12, sizeof(version));
    if (magic != BIOSBOOT_S2HD_MAGIC || version != BIOSBOOT_S2HD_VERSION) {
        return 0;
    }
    if ((uint64_t)fileSize != stage2Len) {
        return 0;
    }
    uint32_t sectors = (uint32_t)((stage2Len + BIOSBOOT_SECTOR_SIZE - 1) / BIOSBOOT_SECTOR_SIZE);
    if (sectors == 0 || sectors > BIOSBOOT_STAGE2_MAX_SECTORS) {
        return 0;
    }
    return sectors;
}
