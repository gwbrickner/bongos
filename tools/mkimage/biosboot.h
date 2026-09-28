/* Installs BIOS stage1/stage2 into the disk image (ARCHITECTURE §5.1/§5.6, D-099). Byte layouts
 * are in docs/specs/bios-boot.md. Pure (no file I/O) -- main.c does the actual reading/writing;
 * this is just the validate-and-patch logic, host-tested on its own. */
#ifndef MKIMAGE_BIOSBOOT_H
#define MKIMAGE_BIOSBOOT_H

#include <stddef.h>
#include <stdint.h>

#define BIOSBOOT_STAGE1_SIZE       440u
#define BIOSBOOT_PATCH_OFFSET      0x1A8u /* within stage1/the protective MBR's boot-code area */
#define BIOSBOOT_S1PB_MAGIC        0x42503153u /* "S1PB" */
#define BIOSBOOT_S2HD_MAGIC        0x44483253u /* "S2HD" */
#define BIOSBOOT_S2HD_VERSION      1u
#define BIOSBOOT_SECTOR_SIZE       512u
#define BIOSBOOT_STAGE2_MAX_SECTORS 832u /* 416 KiB: min(1 MiB BIOS boot partition, this cap) */

/* Validates the S1PB patch-block magic at BIOSBOOT_PATCH_OFFSET inside `stage1` (exactly
 * BIOSBOOT_STAGE1_SIZE bytes) and that `stage2Sectors` fits BIOSBOOT_STAGE2_MAX_SECTORS, then
 * patches `stage2Lba`/`stage2Sectors` (and zeroes the block's reserved field) into `stage1` in
 * place. Returns 0 on success, -1 if the magic doesn't match (stage1 wasn't assembled with a
 * patch block at the expected offset) or `stage2Sectors` is 0 or too large. */
int mkimagePatchStage1(uint8_t stage1[BIOSBOOT_STAGE1_SIZE], uint64_t stage2Lba,
                       uint32_t stage2Sectors);

/* Validates `stage2`'s header (the S2HD magic, headerVersion == BIOSBOOT_S2HD_VERSION, and the
 * header's own `fileSize` field matching `stage2Len` exactly -- catching a stale/mismatched build
 * artifact) and that the sector count `stage2Len` rounds up to fits BIOSBOOT_STAGE2_MAX_SECTORS.
 * Returns that sector count (>= 1) on success, 0 on any validation failure. */
uint32_t mkimageValidateStage2(const uint8_t *stage2, size_t stage2Len);

#endif
