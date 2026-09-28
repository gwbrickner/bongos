/* A read-only FAT32 reader (Microsoft FAT spec v1.03, D-106) over a BootBlockDev, so stage2 can
 * read `/bong/boot.cfg`/`kernel.elf` from the ESP the same way boot/uefi/file.c does through
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL. FSInfo and the dirty-bit fields are ignored -- this loader
 * never writes anything. */
#ifndef BOOT_COMMON_BOOTFAT_H
#define BOOT_COMMON_BOOTFAT_H

#include <stdint.h>

#include "boot-status.h"
#include "bootblk.h"

typedef struct {
    const BootBlockDev *dev;
    uint64_t fatLba;  /* absolute LBA of the active FAT's first sector */
    uint64_t dataLba; /* absolute LBA of cluster 2's first sector */
    uint32_t bytesPerSec;
    uint32_t secPerClus;
    uint32_t fatSz32; /* sectors per FAT (needed to bound a FAT-sector read) */
    uint32_t rootClus;
    uint32_t countOfClusters; /* bounds every chain walk */
} BootFatVol;

typedef struct {
    uint32_t firstCluster;
    uint32_t size;
} BootFatFile;

/* Reads and validates the BPB/FAT32 fields at `partLba` (the partition's first sector): the
 * jmpBoot opcode, BytsPerSec == dev->sectorSize, SecPerClus a power of two in [1,128], RsvdSecCnt
 * >= 1, NumFATs >= 1, the FAT16-shaped fields (RootEntCnt/TotSec16/FATSz16) all zero (this is a
 * FAT32 volume, not FAT12/16), FATSz32 != 0, FSVer == 0, TotSec32 <= partSectors, the 0x55AA boot
 * signature, and a CountOfClusters (computed from these fields) in FAT32's defined range
 * ([65525, 0x0FFFFFF5)). Honors ExtFlags bit 7 (a non-mirrored FAT) to pick the active FAT.
 * `scratch` must be at least dev->sectorSize bytes. No locks, boot-time or host-test only. */
BootStatus bootFatMount(BootFatVol *vol, const BootBlockDev *dev, uint64_t partLba,
                        uint64_t partSectors, uint8_t *scratch);

/* Walks `path` (must start with '/', no empty components, at most 16 components, each at most
 * 255 bytes) from the root directory. Each component matches, ASCII case-insensitively, either a
 * checksum-verified VFAT long name (any non-ASCII UTF-16 code unit in the on-disk name makes that
 * long name never match, per D-106) or the rendered 8.3 short name ("BASE" or "BASE.EXT").
 * Deleted entries (0xE5) and volume-label entries (ATTR_VOLUME_ID, not the LFN attribute) are
 * skipped. Every component but the last must resolve to a directory; the last must resolve to a
 * (non-directory) file. Every cluster-chain walk (including the directories along the path) is
 * bounded by `vol->countOfClusters` steps, so a corrupt or self-looping chain can't hang this
 * function. `scratch` must be at least `vol->bytesPerSec` bytes. Returns BOOT_ERR_NOT_FOUND if
 * any component doesn't resolve, BOOT_ERR_FAT on a structural problem (bad chain, a
 * non-directory in a non-final position), or BOOT_ERR_IO on a read failure. No locks, boot-time
 * or host-test only. */
BootStatus bootFatOpen(const BootFatVol *vol, const char *path, uint8_t *scratch, BootFatFile *out);

/* Reads exactly `file->size` bytes into `dst` (fails with BOOT_ERR_TOO_LARGE if `dstCap` is
 * smaller). Merges contiguous cluster runs into single block-device reads; only the final,
 * possibly-partial sector is routed through `scratch` (at least `vol->bytesPerSec` bytes) so a
 * short last read never writes past `dst[0..file->size)`. A chain shorter than
 * `ceil(file->size / (secPerClus*bytesPerSec))` is BOOT_ERR_FAT. No locks, boot-time or host-test
 * only. */
BootStatus bootFatRead(const BootFatVol *vol, const BootFatFile *file, uint8_t *dst,
                       uint64_t dstCap, uint8_t *scratch);

#endif
