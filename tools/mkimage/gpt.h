/* GPT disk image construction (ARCHITECTURE §5.1, D-008), written from the UEFI Spec §5
 * (GUID Partition Table Format), no third-party GPT library. Sector size is fixed at 512 bytes;
 * the build host is little-endian x86_64 (ARCHITECTURE §1.1), so on-disk fields are written with
 * the host's native byte order and no explicit byteswapping. */
#ifndef MKIMAGE_GPT_H
#define MKIMAGE_GPT_H

#include <stddef.h>
#include <stdint.h>

#define GPT_SECTOR_SIZE           512
#define GPT_PARTITION_ENTRY_COUNT 128
#define GPT_PARTITION_ENTRY_SIZE  128
/* Sectors occupied by the partition entry array: 128 * 128 / 512. */
#define GPT_PARTITION_ARRAY_SECTORS                                                                \
    ((GPT_PARTITION_ENTRY_COUNT * GPT_PARTITION_ENTRY_SIZE) / GPT_SECTOR_SIZE)

/* Mixed-endian EFI/Microsoft GUID: Data1/2/3 are little-endian integers, Data4 is 8 raw bytes in
 * the order the spec/RFC 4122 text form prints them. Matches boot/uefi/include/efi/base.h's
 * EFI_GUID layout; kept as a separate type here since this is host tooling, built with the host
 * clang rather than the cross UEFI target. */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} GptGuid;

/* EFI System Partition, UEFI Spec §5.3.3. */
extern const GptGuid GPT_GUID_ESP;
/* BIOS boot partition (the GRUB-originated convention ARCHITECTURE §5.1 reuses for our own
 * stage2). */
extern const GptGuid GPT_GUID_BIOS_BOOT;
/* D-056: the OS-root-partition *role*, not a filesystem identifier -- a partition with this type
 * holds whatever ARCHITECTURE §5.1/§15.2 says root looks like right now (currently: a bongfs
 * superblock, or, once full-disk encryption exists, a crypt header wrapping one; identify the
 * actual contents by their own magic, never by this GUID). A partition with this type and no
 * recognized magic is unformatted -- never treat that as corruption, and never auto-format it.
 * The kernel finds root by scanning for this type GUID only on the disk whose GPT DiskGUID
 * matches BootInfo.bootDiskGuid, never by scanning every disk. */
extern const GptGuid GPT_TYPE_GUID_ROOT;
/* D-056: same role-not-format reasoning, for the optional swap partition (ARCHITECTURE §5.1 row
 * 4). Not used by mkimage yet (swap arrives in M7.8); minted alongside GPT_TYPE_GUID_ROOT so both
 * format constants get owner review together. */
extern const GptGuid GPT_TYPE_GUID_SWAP;

/* Fills `out` with a random, RFC 4122 version-4 GUID from the OS CSPRNG (/dev/urandom). Used for
 * the disk GUID and each partition's unique GUID -- never for a partition *type* GUID, which
 * must be one of the well-known constants above. */
void gptRandomGuid(GptGuid *out);

typedef struct {
    GptGuid typeGuid;
    GptGuid uniqueGuid;
    uint64_t startLba;
    uint64_t endLba;  /* inclusive */
    const char *name; /* ASCII; converted to UTF-16LE and truncated to 35 chars + NUL */
} GptPartitionSpec;

/* Writes the protective MBR (LBA 0), the primary GPT header and partition array (LBA 1 and
 * onward), and the backup partition array and header (the last GPT_PARTITION_ARRAY_SECTORS + 1
 * LBAs of the image), into `image`. `image` must already be `totalSectors * GPT_SECTOR_SIZE`
 * bytes, zero-initialized; this function does not touch partition payload data, only the MBR/GPT
 * metadata regions. */
void gptWriteLayout(uint8_t *image, uint64_t totalSectors, const GptGuid *diskGuid,
                    const GptPartitionSpec *partitions, size_t partitionCount);

/* The first/last LBA a partition may occupy, given the fixed primary/backup metadata regions
 * gptWriteLayout() reserves. Exposed so a caller can lay out partitions without duplicating
 * gptWriteLayout()'s own arithmetic. */
uint64_t gptFirstUsableLba(void);
uint64_t gptLastUsableLba(uint64_t totalSectors);

#endif
