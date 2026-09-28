/* A small, dumb block-device abstraction (D-105), so bootgpt.c/bootfat.c don't know how reads
 * actually happen -- stage2 backs this with thunked INT 13h reads (chunking/bouncing/retrying
 * internally); host tests back it with a fake in-memory device; a future non-BIOS consumer could
 * back it with anything else. */
#ifndef BOOT_COMMON_BOOTBLK_H
#define BOOT_COMMON_BOOTBLK_H

#include <stdint.h>

#include "boot-status.h"

/* Reads `count` sectors starting at `lba` into `dst` (`count * sectorSize` bytes). Returns
 * BOOT_ERR_IO on any failure. The implementation owns whatever chunking/bouncing/retrying its
 * underlying transport needs -- callers never see that. */
typedef BootStatus (*BootBlockReadFn)(void *ctx, uint64_t lba, uint32_t count, void *dst);

typedef struct {
    BootBlockReadFn read;
    void *ctx;
    uint32_t sectorSize;
    uint64_t sectorCount; /* 0 = unknown */
} BootBlockDev;

#endif
