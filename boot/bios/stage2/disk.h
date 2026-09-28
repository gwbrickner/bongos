/* BIOS disk I/O (D-101/D-105, docs/specs/bios-boot.md §3): a BootBlockDev backed by thunked
 * INT 13h EDD extended reads (AH=42h), so bootgpt.c/bootfat.c can read the real boot disk without
 * knowing anything about real mode. */
#ifndef BOOT_BIOS_STAGE2_DISK_H
#define BOOT_BIOS_STAGE2_DISK_H

#include "boot-status.h"
#include "bootblk.h"

/* Checks EDD extensions are present (INT 13h AH=41h) and reads drive parameters (AH=48h, for
 * `dev->sectorCount`), then fills `dev` (read = the thunked AH=42h reader, ctx = NULL,
 * sectorSize = 512) ready to hand to bootGptFindPartition()/bootFatMount(). Uses the boot drive
 * number entry16 captured from DL at stage1's handoff (docs/specs/bios-boot.md §2 step 9).
 * Returns BOOT_ERR_IO if EDD extensions aren't supported or either BIOS call fails. No locks,
 * boot-time only; not reentrant (shares rmInt()'s scratch area). */
BootStatus diskProbe(BootBlockDev *dev);

#endif
