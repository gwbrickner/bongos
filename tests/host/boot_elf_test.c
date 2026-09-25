/* Host tests for boot/common/elf.c's ELF64 loader (ARCHITECTURE §5.5/§23). Builds synthetic
 * ELF64 byte arrays by hand (matching the on-disk layout elf.c itself parses) rather than
 * shipping real object files, so every rejection path is exercised precisely. */
#include "bootinfo.h"
#include "elf64.h"
#include "framework/test.h"

#include <string.h>

#define TEST_ELF_BUF_SIZE 0x8000
#define KBASE             BOOTINFO_KERNEL_WINDOW_BASE

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} TestPhdr;

static void writeU16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}
static void writeU32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}
static void writeU64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

/* Fills `buf` (TEST_ELF_BUF_SIZE bytes, zeroed first) with a valid ELF64 header and `n` program
 * headers; payload bytes are whatever `buf` already had at that offset (callers needing specific
 * payload content write it after calling this). */
static void buildElf(uint8_t *buf, uint64_t entry, const TestPhdr *phdrs, uint32_t n) {
    memset(buf, 0, TEST_ELF_BUF_SIZE);
    uint64_t phOff = 64;

    buf[0] = 0x7F;
    buf[1] = 'E';
    buf[2] = 'L';
    buf[3] = 'F';
    buf[4] = 2;             /* ELFCLASS64 */
    buf[5] = 1;             /* ELFDATA2LSB */
    buf[6] = 1;             /* EV_CURRENT */
    writeU16(buf + 16, 2);  /* ET_EXEC */
    writeU16(buf + 18, 62); /* EM_X86_64 */
    writeU32(buf + 20, 1);
    writeU64(buf + 24, entry);
    writeU64(buf + 32, phOff);
    writeU64(buf + 40, 0);
    writeU32(buf + 48, 0);
    writeU16(buf + 52, 64);
    writeU16(buf + 54, 56);
    writeU16(buf + 56, (uint16_t)n);
    writeU16(buf + 58, 0);
    writeU16(buf + 60, 0);
    writeU16(buf + 62, 0);

    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = buf + phOff + (uint64_t)i * 56;
        writeU32(p + 0, phdrs[i].type);
        writeU32(p + 4, phdrs[i].flags);
        writeU64(p + 8, phdrs[i].offset);
        writeU64(p + 16, phdrs[i].vaddr);
        writeU64(p + 24, phdrs[i].paddr);
        writeU64(p + 32, phdrs[i].filesz);
        writeU64(p + 40, phdrs[i].memsz);
        writeU64(p + 48, phdrs[i].align);
    }
}

#define PT_LOAD_T   1u
#define PT_INTERP_T 3u

TEST(elfParseAcceptsGoodTwoSegmentImage) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[2] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, KBASE, KBASE, 64, 0x1000, 0x1000},
        {PT_LOAD_T, ELF_PF_R | ELF_PF_W, 0x3000, KBASE + 0x1000, KBASE + 0x1000, 16, 0x1000,
         0x1000},
    };
    buildElf(buf, KBASE, phdrs, 2);

    ElfImage img;
    BootStatus st = elfParse(buf, sizeof(buf), &img);
    ASSERT_EQ(st, BOOT_OK);
    ASSERT_EQ(img.segCount, 2u);
    ASSERT_EQ(img.linkBase, KBASE);
    ASSERT_EQ(img.span, 0x2000ULL);
    ASSERT_EQ(img.entry, KBASE);
}

TEST(elfLoadCopiesDataAndZerosBss) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x100, KBASE, KBASE, 8, 0x1000, 0x1000},
    };
    buildElf(buf, KBASE, phdrs, 1);
    memcpy(buf + 0x100, "DEADBEEF", 8);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_OK);

    uint8_t dest[0x1000];
    memset(dest, 0xAA, sizeof(dest)); /* poison, so a missed zero-fill would be caught */
    ASSERT_EQ(elfLoad(&img, buf, dest), BOOT_OK);
    ASSERT_TRUE(memcmp(dest, "DEADBEEF", 8) == 0);
    for (size_t i = 8; i < sizeof(dest); i++) {
        ASSERT_EQ(dest[i], 0);
    }
}

TEST(elfParseRejectsBadMagic) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, KBASE, KBASE, 16, 0x1000, 0x1000}};
    buildElf(buf, KBASE, phdrs, 1);
    buf[0] = 0x00;

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_HEADER);
}

TEST(elfParseRejectsWrongMachine) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, KBASE, KBASE, 16, 0x1000, 0x1000}};
    buildElf(buf, KBASE, phdrs, 1);
    writeU16(buf + 18, 3); /* EM_386, not EM_X86_64 */

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_HEADER);
}

TEST(elfParseRejectsTooManySegments) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[9];
    for (uint32_t i = 0; i < 9; i++) {
        phdrs[i] = (TestPhdr){
            PT_LOAD_T, ELF_PF_R, 0x2000, KBASE + (uint64_t)i * 0x1000, KBASE + (uint64_t)i * 0x1000,
            0,         0x1000,   0x1000};
    }
    buildElf(buf, KBASE, phdrs, 9); /* ELF_MAX_SEGMENTS is 8 */

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_SEGMENT);
}

TEST(elfParseRejectsWritableAndExecutableSegment) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_W | ELF_PF_X, 0x2000, KBASE, KBASE, 16, 0x1000, 0x1000}};
    buildElf(buf, KBASE, phdrs, 1);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_WX);
}

TEST(elfParseRejectsSegmentOutsideKernelWindow) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, 0x1000, 0x1000, 16, 0x1000, 0x1000}};
    buildElf(buf, 0x1000, phdrs, 1);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_RANGE);
}

TEST(elfParseRejectsSegmentAtOrAboveKernelWindowEnd) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    /* vaddr == BOOTINFO_KERNEL_WINDOW_END: entirely outside the window (the window is
     * half-open, [BASE, END)), must be rejected outright rather than accepted because
     * END - vaddr happens to be 0. */
    TestPhdr phdrs[1] = {{PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, BOOTINFO_KERNEL_WINDOW_END,
                          BOOTINFO_KERNEL_WINDOW_END, 16, 0x1000, 0x1000}};
    buildElf(buf, BOOTINFO_KERNEL_WINDOW_END, phdrs, 1);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_RANGE);
}

TEST(elfParseRejectsSegmentWhoseEndWrapsAroundTheWindowCheck) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    /* vaddr is far above the window (near 2^64), memsz=0x2000: BOOTINFO_KERNEL_WINDOW_END -
     * vaddr underflows to a huge unsigned value if vaddr >= END isn't rejected first, which
     * would make `memsz > (huge value)` false and wrongly accept the segment. */
    uint64_t vaddr = 0xFFFFFFFFFFFFF000ULL;
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, vaddr, vaddr, 0x2000, 0x2000, 0x1000}};
    buildElf(buf, vaddr, phdrs, 1);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_RANGE);
}

TEST(elfParseRejectsSegmentPastWindowEndWithoutWrapping) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    /* The actual pre-fix exploit shape: vaddr a page past the window's end, with a memsz small
     * enough that `END - vaddr` (before the fix, computed even though vaddr > END) doesn't
     * underflow at all -- it's just a small positive number bigger than memsz, so the old
     * `memsz > END - vaddr` comparison alone accepted this segment outright (BOOT_OK), no wrap
     * needed. Only checking `vaddr >= END` before doing that subtraction rejects it. */
    uint64_t vaddr = BOOTINFO_KERNEL_WINDOW_END + 0x1000;
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, vaddr, vaddr, 16, 0x1000, 0x1000}};
    buildElf(buf, vaddr, phdrs, 1);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_RANGE);
}

TEST(elfParseRejectsSegmentCrossingWindowEnd) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    /* In-window vaddr, but vaddr+memsz crosses BOOTINFO_KERNEL_WINDOW_END: exercises the
     * `memsz > END - vaddr` branch itself (as opposed to the `vaddr >= END` branch the two tests
     * above cover), on a subtraction that's valid (vaddr < END here, so it can't underflow). */
    uint64_t vaddr = BOOTINFO_KERNEL_WINDOW_END - 0x1000;
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, vaddr, vaddr, 16, 0x2000, 0x1000}};
    buildElf(buf, vaddr, phdrs, 1);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_RANGE);
}

TEST(elfParseRejectsUnalignedVaddr) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[1] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, KBASE + 1, KBASE + 1, 16, 0x1000, 0x1000}};
    buildElf(buf, KBASE + 1, phdrs, 1);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_SEGMENT);
}

TEST(elfParseRejectsOverlappingSegments) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[2] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, KBASE, KBASE, 16, 0x1000, 0x1000},
        {PT_LOAD_T, ELF_PF_R | ELF_PF_W, 0x3000, KBASE + 0x800, KBASE + 0x800, 16, 0x1000, 0x1000},
    };
    buildElf(buf, KBASE, phdrs, 2);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_SEGMENT);
}

TEST(elfParseRejectsEntryOutsideExecutableSegment) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[2] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, KBASE, KBASE, 16, 0x1000, 0x1000},
        {PT_LOAD_T, ELF_PF_R | ELF_PF_W, 0x3000, KBASE + 0x1000, KBASE + 0x1000, 16, 0x1000,
         0x1000},
    };
    buildElf(buf, KBASE + 0x1000, phdrs, 2); /* entry lands in the RW (non-executable) segment */

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_ENTRY);
}

TEST(elfParseRejectsPtInterpSegment) {
    uint8_t buf[TEST_ELF_BUF_SIZE];
    TestPhdr phdrs[2] = {
        {PT_LOAD_T, ELF_PF_R | ELF_PF_X, 0x2000, KBASE, KBASE, 16, 0x1000, 0x1000},
        {PT_INTERP_T, ELF_PF_R, 0x3000, 0, 0, 4, 4, 1},
    };
    buildElf(buf, KBASE, phdrs, 2);

    ElfImage img;
    ASSERT_EQ(elfParse(buf, sizeof(buf), &img), BOOT_ERR_ELF_SEGMENT);
}
