/* Host tests for boot/common/bootfat.c: a read-only FAT32 reader (D-106). Builds a real, minimal
 * FAT32 volume by hand (a small in-test formatter -- there is no third-party FAT32 library to
 * borrow one from, ARCHITECTURE §0) rather than reusing any on-disk struct from bootfat.c itself,
 * so a layout bug there can't hide from its own test. One sector per cluster (512 B) and exactly
 * 65525 clusters (FAT32's minimum) keeps the image real but small (~32 MiB, calloc'd). */
#include "bootfat.h"
#include "framework/test.h"

#include <stdlib.h>
#include <string.h>

#define SECTOR         512u
#define RSVD_SECS      32u
#define NUM_FATS       2u
#define COUNT_CLUSTERS 65525u /* FAT32's own minimum -- anything less is FAT16-shaped */
#define ROOT_CLUSTER   2u

typedef struct {
    uint8_t *image;
    uint64_t totalSectors;
    uint32_t fatSz32;
    uint32_t nextFreeCluster;
} FatImage;

static BootStatus fakeRead(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    FatImage *f = (FatImage *)ctx;
    if (lba + count > f->totalSectors) {
        return BOOT_ERR_IO;
    }
    memcpy(dst, f->image + lba * SECTOR, (size_t)count * SECTOR);
    return BOOT_OK;
}

static void writeLE16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void writeLE32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t *fatEntryPtr(FatImage *f, uint32_t fatIndex, uint32_t cluster) {
    uint64_t fatLba = RSVD_SECS + (uint64_t)fatIndex * f->fatSz32;
    return f->image + fatLba * SECTOR + (uint64_t)cluster * 4;
}

static void fatSetEntry(FatImage *f, uint32_t cluster, uint32_t value) {
    for (uint32_t i = 0; i < NUM_FATS; i++) {
        writeLE32(fatEntryPtr(f, i, cluster), value & 0x0FFFFFFFu);
    }
}

static uint32_t fatAllocCluster(FatImage *f) {
    uint32_t c = f->nextFreeCluster++;
    fatSetEntry(f, c, 0x0FFFFFFFu); /* EOC by default; overwritten if chained further */
    return c;
}

static uint8_t *clusterPtr(FatImage *f, uint32_t cluster) {
    uint64_t dataLba = RSVD_SECS + (uint64_t)NUM_FATS * f->fatSz32;
    return f->image + (dataLba + (cluster - 2)) * SECTOR;
}

/* Writes one 32-byte short-name directory entry (8.3, no LFN) into `dirCluster` at slot
 * `slotIndex` (0-based; the cluster must have room -- 512/32 = 16 slots). */
static void writeShortDirEntry(FatImage *f, uint32_t dirCluster, uint32_t slotIndex,
                               const char name[11], uint32_t attr, uint32_t firstCluster,
                               uint32_t size) {
    uint8_t *e = clusterPtr(f, dirCluster) + (uint64_t)slotIndex * 32;
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[11] = (uint8_t)attr;
    writeLE16(e + 20, (uint16_t)(firstCluster >> 16));
    writeLE16(e + 26, (uint16_t)firstCluster);
    writeLE32(e + 28, size);
}

static uint8_t shortNameChecksumRef(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + name[i]);
    }
    return sum;
}

/* Writes a two-entry LFN group (enough for names up to 26 chars) plus its short entry, at slots
 * [slotIndex, slotIndex+2]. `shortName` is the raw 11-byte 8.3 alias (must be unique in the dir,
 * unrelated to `longName` other than the checksum link). */
static void writeLfnDirEntry(FatImage *f, uint32_t dirCluster, uint32_t slotIndex,
                             const char *longName, const char shortName[11], uint32_t attr,
                             uint32_t firstCluster, uint32_t size) {
    uint32_t len = (uint32_t)strlen(longName);
    ASSERT_TRUE(len <= 26); /* this helper only builds 2 LFN entries */
    uint8_t chk = shortNameChecksumRef((const uint8_t *)shortName);

    /* Entry order on disk: highest sequence number (the tail of the name) first. */
    uint32_t seq2Len = len > 13 ? len - 13 : 0;
    char buf[27];
    memset(buf, 0xFF, sizeof(buf)); /* 0xFFFF padding past the terminator, byte-doubled below */
    memcpy(buf, longName, len);
    buf[len] = '\0'; /* NUL terminator, then 0xFF padding (approximating 0xFFFF) */

    if (seq2Len > 0) {
        uint8_t *e2 = clusterPtr(f, dirCluster) + (uint64_t)slotIndex * 32;
        memset(e2, 0xFF, 32);
        e2[0] = (uint8_t)(0x40u | 2u); /* last entry, sequence 2 */
        for (uint32_t i = 0; i < 5; i++) {
            uint16_t c = (13 + i < len) ? (uint16_t)(uint8_t)longName[13 + i]
                                        : (13 + i == len ? 0 : 0xFFFFu);
            writeLE16(e2 + 1 + 2 * i, c);
        }
        e2[11] = 0x0F;
        e2[12] = 0;
        e2[13] = chk;
        for (uint32_t i = 0; i < 6; i++) {
            uint16_t c = (18 + i < len) ? (uint16_t)(uint8_t)longName[18 + i]
                                        : (18 + i == len ? 0 : 0xFFFFu);
            writeLE16(e2 + 14 + 2 * i, c);
        }
        writeLE16(e2 + 26, 0);
        for (uint32_t i = 0; i < 2; i++) {
            uint16_t c = (24 + i < len) ? (uint16_t)(uint8_t)longName[24 + i]
                                        : (24 + i == len ? 0 : 0xFFFFu);
            writeLE16(e2 + 28 + 2 * i, c);
        }
        slotIndex++;
    }

    uint8_t *e1 = clusterPtr(f, dirCluster) + (uint64_t)slotIndex * 32;
    memset(e1, 0xFF, 32);
    e1[0] = (uint8_t)(seq2Len > 0 ? 1u : (0x40u | 1u));
    for (uint32_t i = 0; i < 5; i++) {
        uint16_t c = (i < len) ? (uint16_t)(uint8_t)longName[i] : (i == len ? 0 : 0xFFFFu);
        writeLE16(e1 + 1 + 2 * i, c);
    }
    e1[11] = 0x0F;
    e1[12] = 0;
    e1[13] = chk;
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t idx = 5 + i;
        uint16_t c = (idx < len) ? (uint16_t)(uint8_t)longName[idx] : (idx == len ? 0 : 0xFFFFu);
        writeLE16(e1 + 14 + 2 * i, c);
    }
    writeLE16(e1 + 26, 0);
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t idx = 11 + i;
        uint16_t c = (idx < len) ? (uint16_t)(uint8_t)longName[idx] : (idx == len ? 0 : 0xFFFFu);
        writeLE16(e1 + 28 + 2 * i, c);
    }
    slotIndex++;

    writeShortDirEntry(f, dirCluster, slotIndex, shortName, attr, firstCluster, size);
}

static FatImage buildFatImage(void) {
    FatImage f = {0};
    /* fatSz32 = ceil((COUNT_CLUSTERS + 2) * 4 / SECTOR). */
    uint64_t fatBytes = (uint64_t)(COUNT_CLUSTERS + 2) * 4;
    f.fatSz32 = (uint32_t)((fatBytes + SECTOR - 1) / SECTOR);
    f.totalSectors = RSVD_SECS + (uint64_t)NUM_FATS * f.fatSz32 + (uint64_t)COUNT_CLUSTERS;
    f.image = calloc((size_t)f.totalSectors, SECTOR);
    f.nextFreeCluster = ROOT_CLUSTER + 1; /* cluster 2 is pre-assigned to the root dir below */

    uint8_t *bpb = f.image;
    bpb[0] = 0xEB;
    bpb[1] = 0x00;
    bpb[2] = 0x90;
    writeLE16(bpb + 11, (uint16_t)SECTOR);
    bpb[13] = 1; /* SecPerClus */
    writeLE16(bpb + 14, (uint16_t)RSVD_SECS);
    bpb[16] = NUM_FATS;
    writeLE16(bpb + 17, 0);                        /* RootEntCnt */
    writeLE16(bpb + 19, 0);                        /* TotSec16 */
    writeLE16(bpb + 22, 0);                        /* FATSz16 */
    writeLE32(bpb + 32, (uint32_t)f.totalSectors); /* TotSec32 */
    writeLE32(bpb + 36, f.fatSz32);
    writeLE16(bpb + 40, 0); /* ExtFlags: mirrored, FAT 0 active */
    writeLE16(bpb + 42, 0); /* FSVer */
    writeLE32(bpb + 44, ROOT_CLUSTER);
    bpb[510] = 0x55;
    bpb[511] = 0xAA;

    fatSetEntry(&f, 0, 0x0FFFFFF8u);
    fatSetEntry(&f, 1, 0x0FFFFFFFu);
    fatSetEntry(&f, ROOT_CLUSTER, 0x0FFFFFFFu); /* root dir: one cluster, EOC */

    return f;
}

/* `*dev` must outlive `*vol` (bootFatMount stores the pointer) -- both are caller-owned locals,
 * never returned by value, so there's no dangling pointer once this returns. */
static void mountOrFail(FatImage *f, BootBlockDev *dev, BootFatVol *vol) {
    *dev = (BootBlockDev){fakeRead, f, SECTOR, f->totalSectors};
    uint8_t scratch[SECTOR];
    ASSERT_EQ(bootFatMount(vol, dev, 0, f->totalSectors, scratch), BOOT_OK);
}

TEST(bootFatMountRejectsBadSignature) {
    FatImage f = buildFatImage();
    f.image[510] = 0; /* corrupt the 0x55AA signature */
    BootBlockDev dev = {fakeRead, &f, SECTOR, f.totalSectors};
    BootFatVol vol;
    uint8_t scratch[SECTOR];
    ASSERT_EQ(bootFatMount(&vol, &dev, 0, f.totalSectors, scratch), BOOT_ERR_FAT);
    free(f.image);
}

TEST(bootFatOpenAndReadShortNameFile) {
    FatImage f = buildFatImage();
    static const char data[] = "kernel=/bong/kernel.elf\n";
    uint32_t fileClus = fatAllocCluster(&f);
    memcpy(clusterPtr(&f, fileClus), data, sizeof(data) - 1);
    writeShortDirEntry(&f, ROOT_CLUSTER, 0, "BOOT    CFG", 0, fileClus, sizeof(data) - 1);

    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/boot.cfg", scratch, &file), BOOT_OK);
    ASSERT_EQ(file.size, (uint32_t)(sizeof(data) - 1));

    uint8_t out[64];
    ASSERT_EQ(bootFatRead(&vol, &file, out, sizeof(out), scratch), BOOT_OK);
    ASSERT_TRUE(memcmp(out, data, sizeof(data) - 1) == 0);

    free(f.image);
}

TEST(bootFatOpenLongNameCaseInsensitive) {
    FatImage f = buildFatImage();
    static const char data[] = "long name contents";
    uint32_t fileClus = fatAllocCluster(&f);
    memcpy(clusterPtr(&f, fileClus), data, sizeof(data) - 1);
    writeLfnDirEntry(&f, ROOT_CLUSTER, 0, "kernel-debug.elf", "KERNEL~1ELF", 0, fileClus,
                     sizeof(data) - 1);

    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/KERNEL-DEBUG.ELF", scratch, &file), BOOT_OK);
    ASSERT_EQ(file.size, (uint32_t)(sizeof(data) - 1));
    free(f.image);
}

TEST(bootFatOpenNestedDirectory) {
    FatImage f = buildFatImage();
    uint32_t dirClus = fatAllocCluster(&f);
    writeShortDirEntry(&f, ROOT_CLUSTER, 0, "BONG       ", 0x10 /* ATTR_DIRECTORY */, dirClus, 0);

    static const char data[] = "ELF DATA";
    uint32_t fileClus = fatAllocCluster(&f);
    memcpy(clusterPtr(&f, fileClus), data, sizeof(data) - 1);
    writeShortDirEntry(&f, dirClus, 0, "KERNEL  ELF", 0, fileClus, sizeof(data) - 1);

    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/bong/kernel.elf", scratch, &file), BOOT_OK);
    ASSERT_EQ(file.size, (uint32_t)(sizeof(data) - 1));
    free(f.image);
}

TEST(bootFatOpenMissingFileNotFound) {
    FatImage f = buildFatImage();
    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/nope.txt", scratch, &file), BOOT_ERR_NOT_FOUND);
    free(f.image);
}

TEST(bootFatOpenIntermediateNonDirectoryFails) {
    FatImage f = buildFatImage();
    uint32_t fileClus = fatAllocCluster(&f);
    writeShortDirEntry(&f, ROOT_CLUSTER, 0, "BONG       ", 0 /* not a directory */, fileClus, 0);
    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/bong/kernel.elf", scratch, &file), BOOT_ERR_FAT);
    free(f.image);
}

TEST(bootFatReadFragmentedChain) {
    FatImage f = buildFatImage();
    /* Two clusters, deliberately non-contiguous (skip one in between), each fully written. */
    uint32_t c1 = fatAllocCluster(&f);
    fatAllocCluster(&f); /* burn a cluster number so c2 isn't c1+1 */
    uint32_t c2 = fatAllocCluster(&f);
    fatSetEntry(&f, c1, c2); /* chain c1 -> c2 (not contiguous) */

    uint8_t part1[SECTOR], part2[SECTOR];
    for (uint32_t i = 0; i < SECTOR; i++) {
        part1[i] = (uint8_t)i;
        part2[i] = (uint8_t)(255 - i);
    }
    memcpy(clusterPtr(&f, c1), part1, SECTOR);
    memcpy(clusterPtr(&f, c2), part2, SECTOR);
    uint32_t size = SECTOR + 100; /* spills partway into the second cluster */
    writeShortDirEntry(&f, ROOT_CLUSTER, 0, "FRAG    BIN", 0, c1, size);

    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/frag.bin", scratch, &file), BOOT_OK);
    ASSERT_EQ(file.size, size);

    uint8_t *out = calloc(1, size);
    ASSERT_EQ(bootFatRead(&vol, &file, out, size, scratch), BOOT_OK);
    ASSERT_TRUE(memcmp(out, part1, SECTOR) == 0);
    ASSERT_TRUE(memcmp(out + SECTOR, part2, 100) == 0);
    free(out);
    free(f.image);
}

TEST(bootFatReadZeroSizeFile) {
    FatImage f = buildFatImage();
    writeShortDirEntry(&f, ROOT_CLUSTER, 0, "EMPTY   TXT", 0, 0, 0);
    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/empty.txt", scratch, &file), BOOT_OK);
    ASSERT_EQ(file.size, 0u);
    uint8_t out[1];
    ASSERT_EQ(bootFatRead(&vol, &file, out, sizeof(out), scratch), BOOT_OK);
    free(f.image);
}

TEST(bootFatReadRejectsBufferTooSmall) {
    FatImage f = buildFatImage();
    static const char data[] = "0123456789";
    uint32_t fileClus = fatAllocCluster(&f);
    memcpy(clusterPtr(&f, fileClus), data, sizeof(data) - 1);
    writeShortDirEntry(&f, ROOT_CLUSTER, 0, "SMALL   BIN", 0, fileClus, sizeof(data) - 1);
    BootBlockDev dev;
    BootFatVol vol;
    mountOrFail(&f, &dev, &vol);
    uint8_t scratch[SECTOR];
    BootFatFile file;
    ASSERT_EQ(bootFatOpen(&vol, "/small.bin", scratch, &file), BOOT_OK);
    uint8_t out[4];
    ASSERT_EQ(bootFatRead(&vol, &file, out, sizeof(out), scratch), BOOT_ERR_TOO_LARGE);
    free(f.image);
}
