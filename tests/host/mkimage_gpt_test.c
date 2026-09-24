/* Host tests for tools/mkimage's GPT/MBR writer (ARCHITECTURE §23 lists GPT parsing under host
 * tests). Decodes the raw bytes gptWriteLayout() produces by hand, against the UEFI Spec §5
 * layout, rather than reusing gpt.c's own on-disk structs -- so a struct-layout bug in gpt.c
 * can't hide from its own test. */
#include "crc32.h"
#include "framework/test.h"
#include "gpt.h"

#include <stdlib.h>
#include <string.h>

#define SECTOR 512

static uint16_t readLE16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t readLE32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t readLE64(const uint8_t *p) {
    uint64_t lo = readLE32(p);
    uint64_t hi = readLE32(p + 4);
    return lo | (hi << 32);
}

TEST(crc32MatchesKnownVector) {
    /* The standard CRC-32 (reflected 0xEDB88320) check value for "123456789". */
    ASSERT_EQ(crc32Compute("123456789", 9), 0xCBF43926U);
}

TEST(gptLayoutProducesValidHeadersAndBackupMirror) {
    uint64_t totalSectors = 8192; /* 4 MiB: small but big enough for real partitions */
    uint8_t *image = calloc((size_t)totalSectors, SECTOR);
    ASSERT_TRUE(image != NULL);

    GptGuid diskGuid = {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};
    GptGuid partAGuid = {0x44444444, 0x5555, 0x6666, {9, 10, 11, 12, 13, 14, 15, 16}};
    GptGuid partBGuid = {0x77777777, 0x8888, 0x9999, {17, 18, 19, 20, 21, 22, 23, 24}};

    uint64_t firstUsable = gptFirstUsableLba();
    uint64_t lastUsable = gptLastUsableLba(totalSectors);

    GptPartitionSpec partitions[2] = {
        {GPT_GUID_ESP, partAGuid, firstUsable, firstUsable + 99, "PART_A"},
        {GPT_TYPE_GUID_ROOT, partBGuid, firstUsable + 100, lastUsable, "PART_B"},
    };
    gptWriteLayout(image, totalSectors, &diskGuid, partitions, 2);

    /* Protective MBR (LBA 0). */
    ASSERT_EQ(image[510], 0x55);
    ASSERT_EQ(image[511], 0xAA);
    ASSERT_EQ(image[446 + 4], 0xEE);          /* partition type: GPT protective */
    ASSERT_EQ(readLE32(image + 446 + 8), 1U); /* StartingLBA */
    ASSERT_EQ(readLE32(image + 446 + 12), (uint32_t)(totalSectors - 1)); /* SizeInLBA */

    const uint8_t *primaryHeader = image + 1 * SECTOR;
    ASSERT_TRUE(memcmp(primaryHeader, "EFI PART", 8) == 0);
    ASSERT_EQ(readLE32(primaryHeader + 8), 0x00010000U);       /* revision */
    ASSERT_EQ(readLE32(primaryHeader + 12), 92U);              /* header size */
    ASSERT_EQ(readLE64(primaryHeader + 24), 1ULL);             /* MyLBA */
    ASSERT_EQ(readLE64(primaryHeader + 32), totalSectors - 1); /* AlternateLBA */
    ASSERT_EQ(readLE64(primaryHeader + 40), firstUsable);
    ASSERT_EQ(readLE64(primaryHeader + 48), lastUsable);
    ASSERT_EQ(readLE64(primaryHeader + 72), 2ULL); /* PartitionEntryLBA */
    ASSERT_EQ(readLE32(primaryHeader + 80), 128U); /* NumberOfPartitionEntries */
    ASSERT_EQ(readLE32(primaryHeader + 84), 128U); /* SizeOfPartitionEntry */

    /* Header CRC: recompute over the 92-byte header with the CRC field zeroed, must match what
     * was written. */
    uint8_t headerCopy[92];
    memcpy(headerCopy, primaryHeader, sizeof(headerCopy));
    uint32_t storedHeaderCrc = readLE32(headerCopy + 16);
    memset(headerCopy + 16, 0, 4);
    ASSERT_EQ(crc32Compute(headerCopy, sizeof(headerCopy)), storedHeaderCrc);

    /* Partition array CRC: recompute over all 128*128 bytes at LBA 2. */
    const uint8_t *primaryArray = image + 2 * SECTOR;
    uint32_t storedArrayCrc = readLE32(primaryHeader + 88);
    ASSERT_EQ(crc32Compute(primaryArray, 128 * 128), storedArrayCrc);

    /* First partition entry decodes back to what was given: type GUID (mixed-endian: LE
     * Data1/Data2/Data3, then 8 raw Data4 bytes), unique GUID, LBAs, and the UTF-16LE name. */
    ASSERT_EQ(readLE32(primaryArray + 0), GPT_GUID_ESP.data1);
    ASSERT_EQ(readLE16(primaryArray + 4), GPT_GUID_ESP.data2);
    ASSERT_EQ(readLE16(primaryArray + 6), GPT_GUID_ESP.data3);
    ASSERT_TRUE(memcmp(primaryArray + 8, GPT_GUID_ESP.data4, 8) == 0);

    ASSERT_EQ(readLE32(primaryArray + 16), partAGuid.data1);
    ASSERT_EQ(readLE16(primaryArray + 20), partAGuid.data2);
    ASSERT_EQ(readLE16(primaryArray + 22), partAGuid.data3);
    ASSERT_TRUE(memcmp(primaryArray + 24, partAGuid.data4, 8) == 0);

    ASSERT_EQ(readLE64(primaryArray + 32), firstUsable);      /* StartingLBA */
    ASSERT_EQ(readLE64(primaryArray + 40), firstUsable + 99); /* EndingLBA */

    static const char partAName[] = "PART_A";
    for (size_t i = 0; i < sizeof(partAName) - 1; i++) {
        ASSERT_EQ(readLE16(primaryArray + 56 + 2 * i), (uint16_t)(unsigned char)partAName[i]);
    }
    ASSERT_EQ(readLE16(primaryArray + 56 + 2 * (sizeof(partAName) - 1)), 0U); /* NUL terminator */

    /* Backup: header at the last LBA, array immediately before it; MyLBA/AlternateLBA and
     * PartitionEntryLBA point the other way from the primary, everything else matches. */
    uint64_t backupHeaderLba = totalSectors - 1;
    uint64_t backupArrayLba = backupHeaderLba - (128 * 128 / SECTOR);
    const uint8_t *backupHeader = image + backupHeaderLba * SECTOR;
    const uint8_t *backupArray = image + backupArrayLba * SECTOR;

    ASSERT_TRUE(memcmp(backupHeader, "EFI PART", 8) == 0);
    ASSERT_EQ(readLE64(backupHeader + 24), backupHeaderLba); /* MyLBA */
    ASSERT_EQ(readLE64(backupHeader + 32), 1ULL);            /* AlternateLBA */
    ASSERT_EQ(readLE64(backupHeader + 40), firstUsable);
    ASSERT_EQ(readLE64(backupHeader + 48), lastUsable);
    ASSERT_EQ(readLE64(backupHeader + 72), backupArrayLba); /* PartitionEntryLBA */

    uint8_t backupHeaderCopy[92];
    memcpy(backupHeaderCopy, backupHeader, sizeof(backupHeaderCopy));
    uint32_t storedBackupHeaderCrc = readLE32(backupHeaderCopy + 16);
    memset(backupHeaderCopy + 16, 0, 4);
    ASSERT_EQ(crc32Compute(backupHeaderCopy, sizeof(backupHeaderCopy)), storedBackupHeaderCrc);

    ASSERT_TRUE(memcmp(primaryArray, backupArray, 128 * 128) == 0);

    /* Both partitions land strictly inside [firstUsable, lastUsable] and don't overlap. */
    ASSERT_TRUE(firstUsable <= partitions[0].startLba && partitions[0].endLba <= lastUsable);
    ASSERT_TRUE(firstUsable <= partitions[1].startLba && partitions[1].endLba <= lastUsable);
    ASSERT_TRUE(partitions[0].endLba < partitions[1].startLba);

    free(image);
}

TEST(gptRandomGuidSetsRfc4122VersionAndVariant) {
    GptGuid guid;
    gptRandomGuid(&guid);
    ASSERT_EQ(guid.data3 & 0xF000, 0x4000); /* version 4 */
    ASSERT_EQ(guid.data4[0] & 0xC0, 0x80);  /* variant 10xxxxxx */
}
