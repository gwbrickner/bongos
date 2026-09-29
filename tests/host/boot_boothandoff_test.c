/* Host tests for boot/common/boothandoff.c: the shared page-table/BootInfo-build core
 * (ARCHITECTURE §5.5 steps 9-11/§5.6 step 8, D-108). Pool memory comes from aligned_alloc(), same
 * trick as boot_paging_test.c: ptLookup() never dereferences a *leaf* physical address, only the
 * pool-allocated subtables, so a leaf `pa` can be a made-up page-aligned number. */
#include "boothandoff.h"
#include "framework/test.h"

#include <stdlib.h>

#define POOL_PAGES 64

static uint64_t allocPoolPhys(void) {
    void *mem = aligned_alloc(4096, (size_t)POOL_PAGES * 4096);
    return (uint64_t)(uintptr_t)mem;
}

static void freePoolPhys(uint64_t phys) {
    free((void *)(uintptr_t)phys);
}

TEST(bootAllocAddRecordsAndCapsAtMax) {
    BootAllocList list = {0};
    for (uint32_t i = 0; i < BOOT_HANDOFF_MAX_ALLOCS; i++) {
        ASSERT_EQ(bootAllocAdd(&list, i * 0x1000ULL, 1, BOOT_MEM_LOADER_RECLAIM), BOOT_OK);
    }
    ASSERT_EQ(list.count, BOOT_HANDOFF_MAX_ALLOCS);
    ASSERT_EQ(bootAllocAdd(&list, 0, 1, BOOT_MEM_LOADER_RECLAIM), BOOT_ERR_MEMMAP_CAPACITY);
    ASSERT_EQ(list.count, BOOT_HANDOFF_MAX_ALLOCS); /* unchanged on failure */
}

TEST(bootHandoffFillInfoSetsFixedFields) {
    uint8_t seed[64];
    for (int i = 0; i < 64; i++) {
        seed[i] = (uint8_t)i;
    }
    BootHandoffFields fields = {
        .bootMethod = BOOT_METHOD_BIOS,
        .fb = {0},
        .rsdpPhys = 0x1000,
        .kernelPhys = 0x200000,
        .kernelVirtBase = 0xFFFFFFFF80000000ULL,
        .kernelSize = 0x4000,
        .cmdlinePhys = 0x3000,
        .hhdmBase = BOOTINFO_HHDM_BASE,
        .loaderTsc = 12345,
        .efiSystemTablePhys = 0,
        .randomSeed = seed,
    };
    BootInfo bi;
    /* Poison first, so the test can tell a real zero-fill from stale garbage. */
    for (size_t i = 0; i < sizeof(bi); i++) {
        ((uint8_t *)&bi)[i] = 0xAA;
    }
    bootHandoffFillInfo(&bi, &fields);
    ASSERT_EQ(bi.magic, BOOTINFO_MAGIC);
    ASSERT_EQ(bi.version, (uint32_t)BOOTINFO_VERSION);
    ASSERT_EQ(bi.size, (uint32_t)sizeof(BootInfo));
    ASSERT_EQ(bi.bootMethod, (uint32_t)BOOT_METHOD_BIOS);
    ASSERT_EQ(bi.rsdpPhys, 0x1000ULL);
    ASSERT_EQ(bi.kernelPhysBase, 0x200000ULL);
    ASSERT_EQ(bi.kernelVirtBase, 0xFFFFFFFF80000000ULL);
    ASSERT_EQ(bi.kernelSize, 0x4000ULL);
    ASSERT_EQ(bi.kaslrSlide, 0ULL);
    ASSERT_EQ(bi.cmdlinePhys, 0x3000ULL);
    ASSERT_EQ(bi.hhdmBase, (uint64_t)BOOTINFO_HHDM_BASE);
    ASSERT_EQ(bi.loaderTsc, 12345ULL);
    ASSERT_EQ(bi.efiSystemTablePhys, 0ULL);
    ASSERT_EQ(bi.memMapCount, 0u); /* not this function's job -- left zero */
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ((int)bi.randomSeed[i], i);
    }
}

TEST(bootHandoffFinalMapMergesFirmwareInputsAndAllocs) {
    MemMapInput fw[2] = {
        {0x0, 0x2000, BOOT_MEM_USABLE},
        {0x4000, 0x1000, BOOT_MEM_USABLE},
    };
    BootAllocList allocs = {0};
    ASSERT_EQ(bootAllocAdd(&allocs, 0x2000, 1, BOOT_MEM_KERNEL), BOOT_OK);

    MemMapInput work[8];
    uint64_t scratch[16];
    BootMemRegion out[8];
    uint32_t nOut = 0;
    ASSERT_EQ(bootHandoffFinalMap(fw, 2, &allocs, work, 8, scratch, out, 8, &nOut), BOOT_OK);
    /* [0,0x2000) USABLE, [0x2000,0x3000) KERNEL, [0x4000,0x5000) USABLE -- the KERNEL overlay
     * splits the first USABLE run since it sits at its tail. */
    ASSERT_EQ(nOut, 3u);
    ASSERT_EQ(out[0].base, 0x0ULL);
    ASSERT_EQ(out[0].length, 0x2000ULL);
    ASSERT_EQ(out[0].type, (uint32_t)BOOT_MEM_USABLE);
    ASSERT_EQ(out[1].base, 0x2000ULL);
    ASSERT_EQ(out[1].length, 0x1000ULL);
    ASSERT_EQ(out[1].type, (uint32_t)BOOT_MEM_KERNEL);
    ASSERT_EQ(out[2].base, 0x4000ULL);
    ASSERT_EQ(out[2].length, 0x1000ULL);
    ASSERT_EQ(out[2].type, (uint32_t)BOOT_MEM_USABLE);
}

TEST(bootHandoffFinalMapRejectsOverCapacity) {
    MemMapInput fw[1] = {{0, 0x1000, BOOT_MEM_USABLE}};
    BootAllocList allocs = {0};
    ASSERT_EQ(bootAllocAdd(&allocs, 0x1000, 1, BOOT_MEM_KERNEL), BOOT_OK);
    ASSERT_EQ(bootAllocAdd(&allocs, 0x2000, 1, BOOT_MEM_KERNEL), BOOT_OK);
    MemMapInput work[2]; /* too small for 1 fw input + 2 allocs */
    uint64_t scratch[8];
    BootMemRegion out[8];
    uint32_t nOut = 0;
    ASSERT_EQ(bootHandoffFinalMap(fw, 1, &allocs, work, 2, scratch, out, 8, &nOut),
              BOOT_ERR_MEMMAP_CAPACITY);
}

static void makeIdentityElfImage(ElfImage *img, uint64_t linkBase, uint64_t span) {
    img->entry = linkBase;
    img->linkBase = linkBase;
    img->span = span;
    img->segCount = 1;
    img->segs[0].vaddr = linkBase;
    img->segs[0].memsz = span;
    img->segs[0].filesz = span;
    img->segs[0].offset = 0;
    img->segs[0].flags = ELF_PF_X | ELF_PF_R;
}

TEST(bootHandoffMapAllAndSelfCheckRoundTrip) {
    uint64_t poolPhys = allocPoolPhys();
    ASSERT_TRUE(poolPhys != 0);
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, false), BOOT_OK);

    BootMemRegion hhdmRuns[1] = {{0x0, 0x10000, BOOT_MEM_USABLE, 0}};
    ElfImage img;
    uint64_t kernelPhys = 0x100000;
    makeIdentityElfImage(&img, 0xFFFFFFFF80000000ULL, 0x1000);
    uint64_t trampPhys = 0x8000; /* page-aligned, made up -- never dereferenced as a leaf pa */

    BootPtPlan plan = {
        .hhdmRuns = hhdmRuns,
        .hhdmRunCount = 1,
        .elfImage = &img,
        .kernelPhys = kernelPhys,
        .trampPhys = trampPhys,
        .patEntry2Uncacheable = true,
    };
    BootFramebuffer fb = {0}; /* no framebuffer: mapping it is a no-op */
    BootAllocList allocs = {0};
    const char *fbNote = NULL;
    ASSERT_EQ(bootHandoffMapAll(&pt, &plan, &fb, &allocs, &fbNote), BOOT_OK);
    ASSERT_TRUE(fbNote == NULL);
    ASSERT_EQ(allocs.count, 0u); /* no framebuffer overlay recorded */

    /* All inside the mapped HHDM run [0, 0x10000). */
    uint64_t bootInfoVa = BOOTINFO_HHDM_BASE + 0x1000;
    uint64_t cmdlineVa = BOOTINFO_HHDM_BASE + 0x2000;
    uint64_t memMapVa = BOOTINFO_HHDM_BASE + 0x3000;
    uint64_t stackTopVa = BOOTINFO_HHDM_BASE + 0x9000;

    ASSERT_TRUE(bootHandoffSelfCheck(&pt, img.entry, stackTopVa, bootInfoVa, cmdlineVa, memMapVa,
                                     trampPhys, &fb));

    freePoolPhys(poolPhys);
}

TEST(bootHandoffMapAllZeroesFramebufferWhenPatNotUncacheable) {
    uint64_t poolPhys = allocPoolPhys();
    PtBuilder pt;
    ASSERT_EQ(ptInit(&pt, poolPhys, POOL_PAGES, false), BOOT_OK);

    ElfImage img;
    makeIdentityElfImage(&img, 0xFFFFFFFF80000000ULL, 0x1000);
    BootPtPlan plan = {
        .hhdmRuns = NULL,
        .hhdmRunCount = 0,
        .elfImage = &img,
        .kernelPhys = 0x100000,
        .trampPhys = 0x8000,
        .patEntry2Uncacheable = false, /* the framebuffer must be refused */
    };
    BootFramebuffer fb = {.phys = 0xE0000000ULL, .width = 800, .height = 600, .pitch = 3200};
    BootAllocList allocs = {0};
    const char *fbNote = NULL;
    ASSERT_EQ(bootHandoffMapAll(&pt, &plan, &fb, &allocs, &fbNote), BOOT_OK);
    ASSERT_TRUE(fbNote != NULL);
    ASSERT_EQ(fb.phys, 0ULL); /* zeroed: not provided */
    ASSERT_EQ(allocs.count, 0u);

    freePoolPhys(poolPhys);
}
