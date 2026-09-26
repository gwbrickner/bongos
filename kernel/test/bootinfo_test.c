/* ktests for BootInfo validation (ARCHITECTURE §23, ROADMAP M1.3's Done-when check:
 * `bootinfo_valid` must pass). */
#include "bootinfo-validate.h"
#include "cmdline.h"
#include "kernel-boot.h"
#include "ktest.h"
#include "sections.h"

#include <stdbool.h>
#include <stdint.h>

KTEST(bootinfo_valid) {
    const BootInfo *bi = kernelBootInfo();
    const char *why = "";
    /* Not the full bootInfoValidate(): that includes bootInfoCheckRefs(), which asserts the
     * memory-map array/BootInfo page/cmdline all live inside a LOADER_RECLAIM region -- true of
     * the *original* loader handoff (kernelMain already ran the full check on it, once, before
     * pmmReclaimLoaderMemory() ran), but no longer true of kernelBootInfo()'s M2.3 snapshot copy
     * (D-089), whose memMapPhys deliberately points at pmm-owned storage instead. Header shape and
     * memory-map-array sanity are still exactly what the snapshot should satisfy. */
    KTEST_ASSERT(bootInfoCheckHeader(bi, &why) == STATUS_OK);
    KTEST_ASSERT_EQ(bi->version, BOOTINFO_VERSION);
    KTEST_ASSERT_EQ(bi->kernelVirtBase, (uint64_t)(uintptr_t)kernelImageStart);

    uint32_t regionCount = 0;
    const BootMemRegion *regions = kernelBootMemMap(&regionCount);
    KTEST_ASSERT_EQ(regionCount, bi->memMapCount);
    KTEST_ASSERT(bootMemMapCheck(regions, regionCount, &why) == STATUS_OK);
    bool foundKernelRegion = false;
    bool foundUsable = false;
    for (uint32_t i = 0; i < bi->memMapCount; i++) {
        if (regions[i].type == BOOT_MEM_KERNEL && regions[i].base <= bi->kernelPhysBase &&
            bi->kernelPhysBase + bi->kernelSize <= regions[i].base + regions[i].length) {
            foundKernelRegion = true;
        }
        if (regions[i].type == BOOT_MEM_USABLE) {
            foundUsable = true;
        }
    }
    KTEST_ASSERT(foundKernelRegion);
    KTEST_ASSERT(foundUsable);

    char ktestValue[64];
    KTEST_ASSERT(cmdlineFindKtest(kernelCmdline(), ktestValue, sizeof(ktestValue)));
}

KTEST(bootinfo_rejects_bad) {
    const BootInfo *live = kernelBootInfo();
    const char *why = "";

    {
        BootInfo bad = *live;
        bad.magic = 0;
        KTEST_ASSERT(bootInfoCheckHeader(&bad, &why) != STATUS_OK);
    }
    {
        BootInfo bad = *live;
        bad.version = 2;
        KTEST_ASSERT(bootInfoCheckHeader(&bad, &why) != STATUS_OK);
    }
    {
        BootInfo bad = *live;
        bad.size = sizeof(BootInfo) + 1;
        KTEST_ASSERT(bootInfoCheckHeader(&bad, &why) != STATUS_OK);
    }
    {
        BootMemRegion regions[2] = {
            {0x200000, 0x1000, BOOT_MEM_USABLE, 0},
            {0x100000, 0x1000, BOOT_MEM_USABLE, 0},
        };
        KTEST_ASSERT(bootMemMapCheck(regions, 2, &why) != STATUS_OK);
    }
    {
        BootMemRegion regions[2] = {
            {0x100000, 0x2000, BOOT_MEM_USABLE, 0},
            {0x101000, 0x1000, BOOT_MEM_USABLE, 0},
        };
        KTEST_ASSERT(bootMemMapCheck(regions, 2, &why) != STATUS_OK);
    }
    {
        BootMemRegion regions[1] = {{0x100000, 0x1000, 0, 0}};
        KTEST_ASSERT(bootMemMapCheck(regions, 1, &why) != STATUS_OK);
    }
    {
        /* hhdmBase=0 turns every "phys" field below into a literal address (bootInfoCheckRefs()
         * only ever does `hhdmBase + phys` arithmetic, never assumes a real page-table mapping),
         * and giant [0, UINT64_MAX) regions of every type trivially satisfy every containment
         * check except the one this test is actually about: cmdline NUL-termination. */
        char badCmdline[BOOTINFO_CMDLINE_MAX];
        for (uint32_t i = 0; i < BOOTINFO_CMDLINE_MAX; i++) {
            badCmdline[i] = 'a';
        }
        BootMemRegion regions[3] = {
            {0, UINT64_MAX, BOOT_MEM_LOADER_RECLAIM, 0},
            {0, UINT64_MAX, BOOT_MEM_KERNEL, 0},
            {0, UINT64_MAX, BOOT_MEM_USABLE, 0},
        };
        BootInfo bad = *live;
        bad.hhdmBase = 0;
        bad.memMapPhys = (uint64_t)(uintptr_t)regions;
        bad.cmdlinePhys = (uint64_t)(uintptr_t)badCmdline;
        bad.initrdSize = 0;
        bad.fb.phys = 0;
        KTEST_ASSERT(bootInfoCheckRefs(&bad, regions, 3, &why) != STATUS_OK);
    }
}

KTEST(memmap_coalesced) {
    const BootInfo *bi = kernelBootInfo();
    const BootMemRegion *regions =
        (const BootMemRegion *)(uintptr_t)(bi->hhdmBase + bi->memMapPhys);
    for (uint32_t i = 1; i < bi->memMapCount; i++) {
        bool touching = regions[i].base == regions[i - 1].base + regions[i - 1].length;
        bool sameType = regions[i].type == regions[i - 1].type;
        KTEST_ASSERT(!(touching && sameType));
    }
}
