/* GPT partition reader (UEFI Spec §5.3.2/§5.3.3, D-105) over a BootBlockDev. Written from the
 * spec, not shared with tools/mkimage/gpt.c (the disk-image *writer*, host-only, and its own
 * on-disk struct types would collide with this one on tests/host's include path). */
#ifndef BOOT_COMMON_BOOTGPT_H
#define BOOT_COMMON_BOOTGPT_H

#include <stdint.h>

#include "boot-status.h"
#include "bootblk.h"

/* The EFI System Partition's well-known type GUID (UEFI Spec §5.3.3), as its 16 raw on-disk
 * bytes (mixed-endian GUID encoding, not a string). */
extern const uint8_t BOOT_GPT_TYPE_GUID_ESP[16];

typedef struct {
    uint8_t typeGuid[16];
    uint8_t uniqueGuid[16];
    uint64_t startLba, endLba; /* inclusive, both within the header's usable range */
} BootGptPart;

/* Finds the first partition-entry-array entry (in array order) whose type GUID matches
 * `typeGuid` and whose [startLba, endLba] lies inside the GPT header's usable range. Validates
 * the primary header (LBA 1): signature, revision major == 1, 92 <= HeaderSize <= sectorSize, a
 * CRC-32 over the first HeaderSize bytes with the stored checksum field zeroed, MyLBA == 1,
 * FirstUsableLBA <= LastUsableLBA, NumberOfPartitionEntries <= 1024, SizeOfPartitionEntry a power
 * of two in [128, sectorSize] that evenly divides sectorSize, and the partition-entry array's own
 * CRC-32 (streamed through `scratch`, one sector at a time -- never requires the whole array in
 * memory). If the primary fails any of this and `dev->sectorCount != 0`, retries against the
 * backup header at the disk's last LBA. `scratch` must be at least `dev->sectorSize` bytes.
 * Fills `diskGuid` (16 bytes) from whichever header validated. Returns BOOT_ERR_GPT if neither
 * header validates, BOOT_ERR_IO on a read failure, or BOOT_ERR_NOT_FOUND if a header validates
 * but no entry matches. Requires `dev->sectorSize != 0`. No locks, boot-time or host-test only. */
BootStatus bootGptFindPartition(const BootBlockDev *dev, const uint8_t typeGuid[16],
                                uint8_t *scratch, BootGptPart *out, uint8_t diskGuid[16]);

#endif
