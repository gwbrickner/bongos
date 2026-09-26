/* ktests for the portable vmm layer (ARCHITECTURE §6.1/§6.3, D-086..D-090, ROADMAP M2.3):
 * vmmMapKernel/vmmUnmapKernel/the KVA allocator, and that LOADER_RECLAIM was actually reclaimed. */
#include "kernel-boot.h"
#include "ktest.h"
#include "page.h"
#include "pmm.h"
#include "vmm.h"

#include <arch/trap.h>
#include <stdint.h>

static void vmmReadTrigger(void *arg) {
    volatile const uint8_t *p = (volatile const uint8_t *)arg;
    volatile uint8_t v = *p;
    (void)v;
}

/* A full round trip through the public vmm API: allocate a physical page, reserve KVA space for
 * it, map it RW, write through the KVA mapping and confirm the write landed on the right physical
 * page (read back through the HHDM), then unmap and confirm the KVA address no longer resolves to
 * anything (a real #PF, not just a Status code). Also exercises the rejection paths a future
 * vmalloc caller depends on: VMM_EXEC, a VA outside the KVA region, a double map, and the WC-over-
 * RAM anti-aliasing check (D-088). */
KTEST(vmm_map_unmap) {
    const BootInfo *bi = kernelBootInfo();
    Page *page;
    KTEST_ASSERT(pmmAllocPages(0, PMM_FLAG_ZERO, &page) == STATUS_OK);
    uint64_t pa = pmmPageToPhys(page);

    uint64_t va;
    KTEST_ASSERT(vmmKvaAlloc(4096, &va) == STATUS_OK);
    KTEST_ASSERT(vmmMapKernel(va, pa, 4096, VMM_WRITE) == STATUS_OK);

    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)va;
    *p = 0x42;
    const uint8_t *hhdmP = (const uint8_t *)(uintptr_t)(pmmHhdmBase() + pa);
    KTEST_ASSERT_EQ(*hhdmP, 0x42);

    uint64_t outPa;
    VmmFlags outFlags;
    KTEST_ASSERT(vmmLookupKernel(va, &outPa, &outFlags) == STATUS_OK);
    KTEST_ASSERT_EQ(outPa, pa);
    KTEST_ASSERT((outFlags & VMM_WRITE) != 0);

    KTEST_ASSERT(vmmMapKernel(va, pa, 4096, VMM_WRITE) == STATUS_ERR_INVALID); /* already mapped */
    KTEST_ASSERT(vmmMapKernel(va, pa, 4096, VMM_EXEC) == STATUS_ERR_UNSUPPORTED);
    KTEST_ASSERT(vmmMapKernel(0x1000, pa, 4096, VMM_WRITE) == STATUS_ERR_INVALID); /* outside KVA */

    uint64_t va2;
    KTEST_ASSERT(vmmKvaAlloc(4096, &va2) == STATUS_OK);
    KTEST_ASSERT(vmmMapKernel(va2, pa, 4096, VMM_CACHE_WC) == STATUS_ERR_INVALID); /* anti-alias */

    /* D-090 point (g), reachable through the KVA path rather than the HHDM: a writable mapping of
     * the kernel's own text physical range must be refused, or vmmMapKernel would be a second way
     * to defeat W^X the boot-time verifier (which only ever inspects the HHDM alias) never sees.
     */
    KTEST_ASSERT(vmmMapKernel(va2, bi->kernelPhysBase, 4096, VMM_WRITE) == STATUS_ERR_INVALID);

    KTEST_ASSERT(vmmUnmapKernel(va, 4096) == STATUS_OK);
    KTEST_ASSERT(vmmLookupKernel(va, &outPa, &outFlags) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(vmmUnmapKernel(va, 4096) == STATUS_ERR_NOT_FOUND); /* double unmap */

    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_VEC(14), vmmReadTrigger, (void *)(uintptr_t)va, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.cr2, va);

    vmmKvaFree(va, 4096);
    vmmKvaFree(va2, 4096);
    pmmFreePages(page, 0);
}

/* ROADMAP M2.3 step 6 (D-089): every LOADER_RECLAIM page at or above 1 MiB, except the one
 * BootInfo page pmmReclaimLoaderMemory() deliberately keeps, must no longer be RESERVED -- proving
 * the reclaim actually ran and actually freed the right pages, not just that it logged a line. */
KTEST(loader_reclaimed) {
    uint32_t count;
    const BootMemRegion *regions = kernelBootMemMap(&count);
    uint64_t bootInfoPhys = kernelBootInfoPagePhys();
    bool checkedAny = false;

    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].type != BOOT_MEM_LOADER_RECLAIM) {
            continue;
        }
        uint64_t base = regions[i].base;
        uint64_t end = base + regions[i].length;
        if (base < 0x100000) {
            base = 0x100000; /* D-080's low-memory withholding: never reclaimed either way */
        }
        for (uint64_t phys = base; phys < end; phys += 4096) {
            KTEST_ASSERT(pmmPfnValid(phys >> 12));
            Page *p = pmmPhysToPage(phys);
            KTEST_ASSERT(p != NULL);
            if (phys == bootInfoPhys) {
                KTEST_ASSERT_EQ(p->state, PAGE_STATE_RESERVED);
                continue;
            }
            KTEST_ASSERT(p->state != PAGE_STATE_RESERVED);
            checkedAny = true;
        }
    }
    KTEST_ASSERT(checkedAny);
}
