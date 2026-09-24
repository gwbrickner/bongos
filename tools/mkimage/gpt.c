#include "gpt.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "gpt.c writes on-disk fields with the host's native byte order; the build host "
               "must be little-endian (ARCHITECTURE §1.1)");

const GptGuid GPT_GUID_ESP = {
    0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};

const GptGuid GPT_GUID_BIOS_BOOT = {
    0x21686148, 0x6449, 0x6E6F, {0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}};

/* D-056: generated with `cat /proc/sys/kernel/random/uuid` (the kernel's own RFC 4122 v4
 * generator), one call per constant. */
const GptGuid GPT_TYPE_GUID_ROOT = {
    0xD873F840, 0x6583, 0x4B38, {0x8C, 0x8B, 0xFA, 0x27, 0xF8, 0x33, 0x2D, 0x8D}};
const GptGuid GPT_TYPE_GUID_SWAP = {
    0x1FC85B55, 0x4413, 0x4D12, {0x8C, 0x5F, 0xAD, 0x7B, 0xFA, 0xA6, 0xCB, 0x9D}};

void gptRandomGuid(GptGuid *out) {
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom == NULL) {
        fprintf(stderr, "mkimage: cannot open /dev/urandom: %s\n", strerror(errno));
        exit(1);
    }
    uint8_t raw[16];
    if (fread(raw, 1, sizeof(raw), urandom) != sizeof(raw)) {
        fprintf(stderr, "mkimage: short read from /dev/urandom\n");
        fclose(urandom);
        exit(1);
    }
    fclose(urandom);

    memcpy(&out->data1, &raw[0], 4);
    memcpy(&out->data2, &raw[4], 2);
    memcpy(&out->data3, &raw[6], 2);
    memcpy(out->data4, &raw[8], 8);

    /* RFC 4122 version 4 (random) and variant bits, for a well-formed unique GUID. Type GUIDs
     * (GPT_GUID_ESP etc.) are fixed constants and never go through this function. */
    out->data3 = (uint16_t)((out->data3 & 0x0FFF) | 0x4000);
    out->data4[0] = (uint8_t)((out->data4[0] & 0x3F) | 0x80);
}

#pragma pack(push, 1)
typedef struct {
    char signature[8];
    uint32_t revision;
    uint32_t headerSize;
    uint32_t headerCrc32;
    uint32_t reserved;
    uint64_t myLba;
    uint64_t alternateLba;
    uint64_t firstUsableLba;
    uint64_t lastUsableLba;
    GptGuid diskGuid;
    uint64_t partitionEntryLba;
    uint32_t numberOfPartitionEntries;
    uint32_t sizeOfPartitionEntry;
    uint32_t partitionEntryArrayCrc32;
} GptHeaderOnDisk;

typedef struct {
    GptGuid typeGuid;
    GptGuid uniqueGuid;
    uint64_t startingLba;
    uint64_t endingLba;
    uint64_t attributes;
    uint16_t name[36];
} GptPartitionEntryOnDisk;
#pragma pack(pop)

_Static_assert(sizeof(GptHeaderOnDisk) == 92, "GPT header must be exactly 92 bytes");
_Static_assert(sizeof(GptPartitionEntryOnDisk) == GPT_PARTITION_ENTRY_SIZE,
               "GPT partition entry must be exactly 128 bytes");

static void buildPartitionArray(uint8_t *array, const GptPartitionSpec *partitions,
                                size_t partitionCount) {
    memset(array, 0, (size_t)GPT_PARTITION_ENTRY_COUNT * GPT_PARTITION_ENTRY_SIZE);
    for (size_t i = 0; i < partitionCount; i++) {
        GptPartitionEntryOnDisk entry;
        memset(&entry, 0, sizeof(entry));
        entry.typeGuid = partitions[i].typeGuid;
        entry.uniqueGuid = partitions[i].uniqueGuid;
        entry.startingLba = partitions[i].startLba;
        entry.endingLba = partitions[i].endLba;
        entry.attributes = 0;
        size_t nameLen = strlen(partitions[i].name);
        if (nameLen > 35) {
            nameLen = 35;
        }
        for (size_t c = 0; c < nameLen; c++) {
            entry.name[c] = (uint16_t)(unsigned char)partitions[i].name[c];
        }
        memcpy(array + i * GPT_PARTITION_ENTRY_SIZE, &entry, sizeof(entry));
    }
}

static void writeHeader(uint8_t *sector, uint64_t myLba, uint64_t alternateLba,
                        uint64_t firstUsableLba, uint64_t lastUsableLba, const GptGuid *diskGuid,
                        uint64_t partitionEntryLba, uint32_t partitionArrayCrc32) {
    memset(sector, 0, GPT_SECTOR_SIZE);
    GptHeaderOnDisk header;
    memset(&header, 0, sizeof(header));
    memcpy(header.signature, "EFI PART", 8);
    header.revision = 0x00010000;
    header.headerSize = sizeof(GptHeaderOnDisk);
    header.headerCrc32 = 0;
    header.reserved = 0;
    header.myLba = myLba;
    header.alternateLba = alternateLba;
    header.firstUsableLba = firstUsableLba;
    header.lastUsableLba = lastUsableLba;
    header.diskGuid = *diskGuid;
    header.partitionEntryLba = partitionEntryLba;
    header.numberOfPartitionEntries = GPT_PARTITION_ENTRY_COUNT;
    header.sizeOfPartitionEntry = GPT_PARTITION_ENTRY_SIZE;
    header.partitionEntryArrayCrc32 = partitionArrayCrc32;
    header.headerCrc32 = crc32Compute(&header, sizeof(header));
    memcpy(sector, &header, sizeof(header));
}

static void writeProtectiveMbr(uint8_t *sector, uint64_t totalSectors) {
    memset(sector, 0, GPT_SECTOR_SIZE);
    /* Bytes 0-439 (boot code) and 440-445 (disk signature + reserved) stay zero until the BIOS
     * stage1 (M2.5) patches this region. */
    uint8_t *entry = sector + 446;
    entry[0] = 0x00; /* not the active/boot partition */
    entry[1] = 0x00; /* CHS start head */
    entry[2] = 0x02; /* CHS start sector/cylinder, per UEFI Spec Table 5.3 */
    entry[3] = 0x00;
    entry[4] = 0xEE; /* partition type: GPT protective */
    entry[5] = 0xFF; /* CHS end (max, unused) */
    entry[6] = 0xFF;
    entry[7] = 0xFF;
    uint32_t startingLba = 1;
    memcpy(entry + 8, &startingLba, 4);
    uint64_t sizeInLba = totalSectors - 1;
    uint32_t sizeField = sizeInLba > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)sizeInLba;
    memcpy(entry + 12, &sizeField, 4);
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

uint64_t gptFirstUsableLba(void) {
    return 2 + GPT_PARTITION_ARRAY_SECTORS;
}

uint64_t gptLastUsableLba(uint64_t totalSectors) {
    uint64_t backupHeaderLba = totalSectors - 1;
    uint64_t backupArrayLba = backupHeaderLba - GPT_PARTITION_ARRAY_SECTORS;
    return backupArrayLba - 1;
}

void gptWriteLayout(uint8_t *image, uint64_t totalSectors, const GptGuid *diskGuid,
                    const GptPartitionSpec *partitions, size_t partitionCount) {
    /* A disk with no room for the primary + backup metadata at all is a caller bug, not a user
     * mistake to report nicely -- main.c's own --size-mib validation is what a bad CLI argument
     * hits first. gptLastUsableLba() would silently underflow past this point. */
    assert(totalSectors >= 2 * (2 + GPT_PARTITION_ARRAY_SECTORS) &&
           "totalSectors too small to hold the primary and backup GPT metadata");
    assert(partitionCount <= GPT_PARTITION_ENTRY_COUNT);

    uint64_t primaryArrayLba = 2;
    uint64_t firstUsableLba = gptFirstUsableLba();
    uint64_t backupHeaderLba = totalSectors - 1;
    uint64_t backupArrayLba = backupHeaderLba - GPT_PARTITION_ARRAY_SECTORS;
    uint64_t lastUsableLba = gptLastUsableLba(totalSectors);

    for (size_t i = 0; i < partitionCount; i++) {
        assert(partitions[i].startLba <= partitions[i].endLba && "partition start after end");
        assert(partitions[i].startLba >= firstUsableLba && partitions[i].endLba <= lastUsableLba &&
               "partition outside the usable LBA range");
        for (size_t j = 0; j < i; j++) {
            assert((partitions[i].endLba < partitions[j].startLba ||
                    partitions[i].startLba > partitions[j].endLba) &&
                   "overlapping partitions");
        }
    }

    uint8_t *array = malloc((size_t)GPT_PARTITION_ENTRY_COUNT * GPT_PARTITION_ENTRY_SIZE);
    if (array == NULL) {
        fprintf(stderr, "mkimage: out of memory building the GPT partition array\n");
        exit(1);
    }
    buildPartitionArray(array, partitions, partitionCount);
    uint32_t arrayCrc32 =
        crc32Compute(array, (size_t)GPT_PARTITION_ENTRY_COUNT * GPT_PARTITION_ENTRY_SIZE);

    writeProtectiveMbr(image, totalSectors);

    writeHeader(image + 1 * GPT_SECTOR_SIZE, 1, backupHeaderLba, firstUsableLba, lastUsableLba,
                diskGuid, primaryArrayLba, arrayCrc32);
    memcpy(image + primaryArrayLba * GPT_SECTOR_SIZE, array,
           (size_t)GPT_PARTITION_ENTRY_COUNT * GPT_PARTITION_ENTRY_SIZE);

    memcpy(image + backupArrayLba * GPT_SECTOR_SIZE, array,
           (size_t)GPT_PARTITION_ENTRY_COUNT * GPT_PARTITION_ENTRY_SIZE);
    writeHeader(image + backupHeaderLba * GPT_SECTOR_SIZE, backupHeaderLba, 1, firstUsableLba,
                lastUsableLba, diskGuid, backupArrayLba, arrayCrc32);

    free(array);
}
