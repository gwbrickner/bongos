/* Host tests for boot/common/paging.c's page-table builder (ARCHITECTURE §5.4/§6.3/§23). Pool
 * memory comes from aligned_alloc(); since ptLookup() never dereferences a *leaf* physical
 * address (only ptGetOrAllocTable()'s own pool-allocated subtables), a leaf `pa` can be any
 * page-aligned made-up number -- only the pool itself needs to be real, dereferenceable memory. */
#include "bootinfo.h"
#include "elf64.h"
#include "framework/test.h"
#include "paging.h"

#include <stdlib.h>

#define POOL_PAGES 64

static uint64_t allocPoolPhys(void) {
    void *mem = aligned_alloc(4096, (size_t)POOL_PAGES * 4096);
    return (uint64_t)(uintptr_t)mem;
}

static void freePoolPhys(uint64_t phys) {
    free((void *)(uintptr_t)phys);
}

TEST(pagingRoundTrip4kNoLargePages) {
    uint64_t poolPhys = allocPoolPhys();
    ASSERT_TRUE(poolPhys != 0);
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, false), BOOT_OK);

    uint64_t va = 0x0000123456000000ULL;
    uint64_t pa = 0x0000000012340000ULL;
    ASSERT_EQ(ptMapRange(&pt, va, pa, 4096, PT_FLAGS_KERNEL_RW, true), BOOT_OK);

    uint64_t outPa, outFlags;
    ASSERT_EQ(ptLookup(&pt, va + 0x123, &outPa, &outFlags), BOOT_OK);
    ASSERT_EQ(outPa, pa + 0x123);
    ASSERT_TRUE((outFlags & PT_W) != 0);
    ASSERT_TRUE((outFlags & PT_NX) != 0);

    freePoolPhys(poolPhys);
}

TEST(pagingMaps1GWhenSupported) {
    uint64_t poolPhys = allocPoolPhys();
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, true), BOOT_OK);

    uint64_t va = 0;
    uint64_t pa = 0x40000000ULL; /* 1 GiB aligned */
    ASSERT_EQ(ptMapRange(&pt, va, pa, PT_SIZE_1G, PT_FLAGS_HHDM, true), BOOT_OK);

    uint64_t outPa, outFlags;
    ASSERT_EQ(ptLookup(&pt, va + 0x5000, &outPa, &outFlags), BOOT_OK);
    ASSERT_EQ(outPa, pa + 0x5000);
    ASSERT_TRUE((outFlags & PT_PS) != 0);

    freePoolPhys(poolPhys);
}

/* The only coverage of the 2 MiB fallback path: QEMU's `-cpu max` always advertises PDPE1GB
 * (ARCHITECTURE §1.1), so the real loader never exercises `has1G == false` under `make test`. */
TEST(pagingFallsBackTo2MWithoutPdpe1gb) {
    uint64_t poolPhys = allocPoolPhys();
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, false), BOOT_OK);

    uint64_t va = 0x200000ULL; /* 2 MiB aligned */
    uint64_t pa = 0x200000ULL;
    ASSERT_EQ(ptMapRange(&pt, va, pa, PT_SIZE_2M, PT_FLAGS_HHDM, true), BOOT_OK);

    uint64_t outPa, outFlags;
    ASSERT_EQ(ptLookup(&pt, va + 0x1000, &outPa, &outFlags), BOOT_OK);
    ASSERT_EQ(outPa, pa + 0x1000);
    ASSERT_TRUE((outFlags & PT_PS) != 0);

    freePoolPhys(poolPhys);
}

TEST(pagingDetectsConflict) {
    uint64_t poolPhys = allocPoolPhys();
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, false), BOOT_OK);
    ASSERT_EQ(ptMapRange(&pt, 0x1000, 0x1000, 4096, PT_FLAGS_KERNEL_RO, false), BOOT_OK);
    ASSERT_EQ(ptMapRange(&pt, 0x1000, 0x2000, 4096, PT_FLAGS_KERNEL_RO, false),
              BOOT_ERR_PT_CONFLICT);
    freePoolPhys(poolPhys);
}

TEST(pagingReportsOutOfMemoryWhenPoolExhausted) {
    uint64_t poolPhys = allocPoolPhys();
    PtBuilder pt;
    /* poolPages=1: only room for the PML4 itself, none for the PDPT ptMapRange needs next. */
    ASSERT_EQ(ptInit(&pt, poolPhys, 1, false), BOOT_OK);
    ASSERT_EQ(ptMapRange(&pt, 0x1000, 0x1000, 4096, PT_FLAGS_KERNEL_RO, false), BOOT_ERR_NO_MEMORY);
    freePoolPhys(poolPhys);
}

TEST(pagingLookupReportsNotMapped) {
    uint64_t poolPhys = allocPoolPhys();
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, false), BOOT_OK);
    uint64_t outPa, outFlags;
    ASSERT_EQ(ptLookup(&pt, 0x123456000ULL, &outPa, &outFlags), BOOT_ERR_NOT_MAPPED);
    freePoolPhys(poolPhys);
}

TEST(pagingMapsElfImageWithPerSegmentFlags) {
    uint64_t poolPhys = allocPoolPhys();
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, false), BOOT_OK);

    ElfImage img;
    img.entry = BOOTINFO_KERNEL_WINDOW_BASE;
    img.linkBase = BOOTINFO_KERNEL_WINDOW_BASE;
    img.span = 0x2000;
    img.segCount = 2;
    img.segs[0] =
        (ElfSegment){BOOTINFO_KERNEL_WINDOW_BASE, 0x1000, 0x1000, 0, ELF_PF_R | ELF_PF_X, 0};
    img.segs[1] = (ElfSegment){
        BOOTINFO_KERNEL_WINDOW_BASE + 0x1000, 0x1000, 0x1000, 0x1000, ELF_PF_R | ELF_PF_W, 0};

    uint64_t physBase = 0x200000;
    ASSERT_EQ(ptMapElfImage(&pt, &img, physBase, 0), BOOT_OK);

    uint64_t outPa, outFlags;
    ASSERT_EQ(ptLookup(&pt, BOOTINFO_KERNEL_WINDOW_BASE, &outPa, &outFlags), BOOT_OK);
    ASSERT_EQ(outPa, physBase);
    ASSERT_TRUE((outFlags & PT_W) == 0);
    ASSERT_TRUE((outFlags & PT_NX) == 0);

    ASSERT_EQ(ptLookup(&pt, BOOTINFO_KERNEL_WINDOW_BASE + 0x1000, &outPa, &outFlags), BOOT_OK);
    ASSERT_EQ(outPa, physBase + 0x1000);
    ASSERT_TRUE((outFlags & PT_W) != 0);
    ASSERT_TRUE((outFlags & PT_NX) != 0);

    freePoolPhys(poolPhys);
}
