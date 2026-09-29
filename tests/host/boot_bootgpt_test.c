/* Host tests for boot/common/bootgpt.c (D-105): builds a real GPT disk image with
 * tools/mkimage's own gptWriteLayout() (the writer), then reads it back with bootGptFindPartition
 * (a completely separate implementation, written from the spec -- see bootgpt.h's header
 * comment), including corrupting the primary to exercise the backup fallback. */
#include "bootgpt.h"
#include "framework/test.h"
#include "gpt.h"

#include <stdlib.h>
#include <string.h>

#define SECTOR 512

typedef struct {
    uint8_t *image;
    uint64_t totalSectors;
} FakeDisk;

static BootStatus fakeRead(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    FakeDisk *d = (FakeDisk *)ctx;
    if (lba + count > d->totalSectors) {
        return BOOT_ERR_IO;
    }
    memcpy(dst, d->image + lba * SECTOR, (size_t)count * SECTOR);
    return BOOT_OK;
}

static FakeDisk buildDisk(uint64_t totalSectors, GptGuid *diskGuidOut, uint64_t *espStartOut,
                          uint64_t *espEndOut) {
    uint8_t *image = calloc((size_t)totalSectors, SECTOR);
    GptGuid diskGuid = {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};
    GptGuid espUnique = {0x44444444, 0x5555, 0x6666, {9, 10, 11, 12, 13, 14, 15, 16}};
    GptGuid rootUnique = {0x77777777, 0x8888, 0x9999, {17, 18, 19, 20, 21, 22, 23, 24}};
    uint64_t firstUsable = gptFirstUsableLba();
    uint64_t lastUsable = gptLastUsableLba(totalSectors);
    uint64_t espEnd = firstUsable + 199;
    GptPartitionSpec partitions[2] = {
        {GPT_GUID_ESP, espUnique, firstUsable, espEnd, "ESP"},
        {GPT_TYPE_GUID_ROOT, rootUnique, espEnd + 1, lastUsable, "ROOT"},
    };
    gptWriteLayout(image, totalSectors, &diskGuid, partitions, 2);
    *diskGuidOut = diskGuid;
    *espStartOut = firstUsable;
    *espEndOut = espEnd;
    FakeDisk d = {image, totalSectors};
    return d;
}

static void guidToRaw(const GptGuid *g, uint8_t out[16]) {
    out[0] = (uint8_t)(g->data1);
    out[1] = (uint8_t)(g->data1 >> 8);
    out[2] = (uint8_t)(g->data1 >> 16);
    out[3] = (uint8_t)(g->data1 >> 24);
    out[4] = (uint8_t)(g->data2);
    out[5] = (uint8_t)(g->data2 >> 8);
    out[6] = (uint8_t)(g->data3);
    out[7] = (uint8_t)(g->data3 >> 8);
    memcpy(out + 8, g->data4, 8);
}

TEST(bootGptFindsEspOnValidPrimary) {
    GptGuid diskGuid;
    uint64_t espStart, espEnd;
    FakeDisk d = buildDisk(8192, &diskGuid, &espStart, &espEnd);
    BootBlockDev dev = {fakeRead, &d, SECTOR, d.totalSectors};

    uint8_t scratch[SECTOR];
    BootGptPart out;
    uint8_t gotDiskGuid[16];
    ASSERT_EQ(bootGptFindPartition(&dev, BOOT_GPT_TYPE_GUID_ESP, scratch, &out, gotDiskGuid),
              BOOT_OK);
    ASSERT_EQ(out.startLba, espStart);
    ASSERT_EQ(out.endLba, espEnd);
    uint8_t rawDiskGuid[16];
    guidToRaw(&diskGuid, rawDiskGuid);
    ASSERT_TRUE(memcmp(gotDiskGuid, rawDiskGuid, 16) == 0);

    free(d.image);
}

TEST(bootGptFallsBackToBackupWhenPrimaryCorrupted) {
    GptGuid diskGuid;
    uint64_t espStart, espEnd;
    FakeDisk d = buildDisk(8192, &diskGuid, &espStart, &espEnd);
    /* Corrupt the primary header's signature. */
    memset(d.image + 1 * SECTOR, 0, 8);
    BootBlockDev dev = {fakeRead, &d, SECTOR, d.totalSectors};

    uint8_t scratch[SECTOR];
    BootGptPart out;
    uint8_t gotDiskGuid[16];
    ASSERT_EQ(bootGptFindPartition(&dev, BOOT_GPT_TYPE_GUID_ESP, scratch, &out, gotDiskGuid),
              BOOT_OK);
    ASSERT_EQ(out.startLba, espStart);
    ASSERT_EQ(out.endLba, espEnd);

    free(d.image);
}

TEST(bootGptFailsWhenBothHeadersCorrupted) {
    GptGuid diskGuid;
    uint64_t espStart, espEnd;
    FakeDisk d = buildDisk(8192, &diskGuid, &espStart, &espEnd);
    memset(d.image + 1 * SECTOR, 0, 8);
    memset(d.image + (d.totalSectors - 1) * SECTOR, 0, 8);
    BootBlockDev dev = {fakeRead, &d, SECTOR, d.totalSectors};

    uint8_t scratch[SECTOR];
    BootGptPart out;
    uint8_t gotDiskGuid[16];
    ASSERT_EQ(bootGptFindPartition(&dev, BOOT_GPT_TYPE_GUID_ESP, scratch, &out, gotDiskGuid),
              BOOT_ERR_GPT);

    free(d.image);
}

TEST(bootGptArrayCrcCorruptionFallsBackToBackup) {
    GptGuid diskGuid;
    uint64_t espStart, espEnd;
    FakeDisk d = buildDisk(8192, &diskGuid, &espStart, &espEnd);
    /* Corrupt one byte of the primary partition array (LBA 2), leaving both headers intact. */
    d.image[2 * SECTOR + 40] ^= 0xFF;
    BootBlockDev dev = {fakeRead, &d, SECTOR, d.totalSectors};

    uint8_t scratch[SECTOR];
    BootGptPart out;
    uint8_t gotDiskGuid[16];
    /* Falls back to the (intact) backup array and still finds it. */
    ASSERT_EQ(bootGptFindPartition(&dev, BOOT_GPT_TYPE_GUID_ESP, scratch, &out, gotDiskGuid),
              BOOT_OK);
    ASSERT_EQ(out.startLba, espStart);

    free(d.image);
}

TEST(bootGptNotFoundDoesNotFallBackToBackup) {
    GptGuid diskGuid;
    uint64_t espStart, espEnd;
    FakeDisk d = buildDisk(8192, &diskGuid, &espStart, &espEnd);
    BootBlockDev dev = {fakeRead, &d, SECTOR, d.totalSectors};

    static const uint8_t neverUsedTypeGuid[16] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
                                                  0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    uint8_t scratch[SECTOR];
    BootGptPart out;
    uint8_t gotDiskGuid[16];
    ASSERT_EQ(bootGptFindPartition(&dev, neverUsedTypeGuid, scratch, &out, gotDiskGuid),
              BOOT_ERR_NOT_FOUND);

    free(d.image);
}

TEST(bootGptEspTypeGuidMatchesSpecBytes) {
    static const uint8_t espBytes[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                         0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
    ASSERT_TRUE(memcmp(BOOT_GPT_TYPE_GUID_ESP, espBytes, 16) == 0);
}
