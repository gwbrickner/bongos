/* ktests for the portable vmm layer (ARCHITECTURE §6.1/§6.3, D-086..D-090, ROADMAP M2.3):
 * vmmMapKernel/vmmUnmapKernel/the KVA allocator, and that LOADER_RECLAIM was actually reclaimed. */
#include "acpi.h"
#include "kernel-boot.h"
#include "ktest.h"
#include "page.h"
#include "pmm.h"
#include "vmalloc.h"
#include "vmm.h"

#include <arch/paging.h>
#include <arch/trap.h>
#include <stdbool.h>
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

/* ROADMAP M2.3 step 6 (D-089, amended by D-123): every LOADER_RECLAIM page at or above 1 MiB must
 * no longer be RESERVED -- proving the reclaim actually ran and actually freed the right pages,
 * not just that it logged a line. Since M2.6 that includes the original BootInfo page, which
 * D-089 used to keep RESERVED: kernelMain wipes the live random seed before vmmInit(), so
 * pmmReclaimLoaderMemory() zeroes and frees that page too. Below 1 MiB (D-080) nothing is
 * reclaimed, BootInfo page included, so such a page is not checked. */
KTEST(loader_reclaimed) {
    uint32_t count;
    const BootMemRegion *regions = kernelBootMemMap(&count);
    uint64_t bootInfoPhys = kernelBootInfoPagePhys();
    bool checkedAny = false;
    bool sawBootInfoPage = false;

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
            KTEST_ASSERT(p->state != PAGE_STATE_RESERVED);
            if (phys == bootInfoPhys) {
                sawBootInfoPage = true; /* D-123: the old carve-out is gone */
            }
            checkedAny = true;
        }
    }
    KTEST_ASSERT(checkedAny);
    /* The BootInfo page is in LOADER_RECLAIM (bootInfoCheckRefs() requires it), so unless the
     * loader placed it below 1 MiB it must have been among the pages just checked. */
    KTEST_ASSERT(bootInfoPhys < 0x100000 || sawBootInfoPage);
}

/* M3.2, D-171: the UC mapping type and vmmMapMmio. Maps the first IOAPIC's registers (a page that
 * is RESERVED, never in the HHDM) and checks the raw PTE is PCD|PWT without the PAT bit; checks
 * that UC refuses RAM (HHDM-WB alias) and that the bogus cache value 3 is rejected. */
KTEST(vmm_map_uc) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK && a->madt.ioapicCount >= 1);
    uint64_t pa = a->madt.ioapics[0].address;

    volatile void *mmio;
    KTEST_ASSERT(vmmMapMmio(pa + 0x10, 4, &mmio) == STATUS_OK);
    uint64_t va = (uint64_t)(uintptr_t)mmio;
    KTEST_ASSERT_EQ(va & 0xFFF, (pa + 0x10) & 0xFFF); /* the sub-page offset is preserved */

    uint64_t outPa;
    VmmFlags outFlags;
    KTEST_ASSERT(vmmLookupKernel(va & ~0xFFFULL, &outPa, &outFlags) == STATUS_OK);
    KTEST_ASSERT_EQ(outPa, pa & ~0xFFFULL);
    KTEST_ASSERT_EQ(outFlags, VMM_WRITE | VMM_CACHE_UC);
    uint64_t raw = archPagingRawPte(va & ~0xFFFULL);
    KTEST_ASSERT((raw & (1ULL << 3)) != 0);  /* PWT */
    KTEST_ASSERT((raw & (1ULL << 4)) != 0);  /* PCD */
    KTEST_ASSERT((raw & (1ULL << 7)) == 0);  /* never the PAT bit */
    KTEST_ASSERT((raw & (1ULL << 63)) != 0); /* NX */

    /* IOAPIC register 0 (ID) reads back; the index register is writable and sticks. */
    volatile uint32_t *regs = (volatile uint32_t *)(uintptr_t)(va - 0x10);
    regs[0] = 1;                         /* IOREGSEL = version register */
    KTEST_ASSERT((regs[4] & 0xFF) != 0); /* IOWIN: version byte is nonzero on any IOAPIC */

    vmmUnmapMmio(mmio, 4);
    KTEST_ASSERT(vmmLookupKernel(va & ~0xFFFULL, NULL, NULL) == STATUS_ERR_NOT_FOUND);

    /* UC over RAM is refused (the HHDM alias is WB), as is size 0 and the bogus cache value. */
    Page *page;
    KTEST_ASSERT(pmmAllocPages(0, PMM_FLAG_ZERO, &page) == STATUS_OK);
    uint64_t ramPa = pmmPageToPhys(page);
    volatile void *bad;
    KTEST_ASSERT(vmmMapMmio(ramPa, 4096, &bad) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapMmio(pa, 0, &bad) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapMmio(UINT64_MAX - 1, 4, &bad) == STATUS_ERR_INVALID);
    uint64_t kva;
    KTEST_ASSERT(vmmKvaAlloc(4096, &kva) == STATUS_OK);
    KTEST_ASSERT(vmmMapKernel(kva, ramPa, 4096, VMM_WRITE | VMM_CACHE_UC) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapKernel(kva, pa, 4096, VMM_WRITE | (3u << 2)) == STATUS_ERR_INVALID);
    vmmKvaFree(kva, 4096);
    pmmFreePages(page, 0);
}

typedef struct {
    volatile void *va;
    uint64_t size;
} MmioUnmapArgs;
static void mmioUnmapTrigger(void *arg) {
    const MmioUnmapArgs *a = (const MmioUnmapArgs *)arg;
    vmmUnmapMmio(a->va, a->size);
}

/* True if every page of `[va & ~0xFFF, va + size)` is still mapped RW UC (vmmMapMmio's own leaf
 * flags). Used to prove a rejected vmmUnmapMmio() left the mapping untouched. */
static bool mmioStillMapped(uint64_t va, uint64_t size) {
    uint64_t base = va & ~0xFFFULL;
    uint64_t end = (va + size + 0xFFF) & ~0xFFFULL;
    for (uint64_t p = base; p < end; p += 4096) {
        VmmFlags f;
        if (vmmLookupKernel(p, NULL, &f) != STATUS_OK || f != (VMM_WRITE | VMM_CACHE_UC)) {
            return false;
        }
    }
    return true;
}

/* vmmUnmapMmio()'s contract (vmm.h): a pointer or size vmmMapMmio() did not hand out is a kernel
 * bug and panics via panicBug() -- before anything is unmapped or freed, so a caught misuse leaves
 * the real mapping intact. Covers a too-small size, an interior pointer, a too-large size, a
 * vmalloc() pointer (a KVA mapping that is not UC MMIO), a pointer outside the KVA region, and a
 * double unmap. */
KTEST(vmm_mmio_unmap_misuse) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK && a->madt.ioapicCount >= 1);
    uint64_t pa = a->madt.ioapics[0].address;

    volatile void *mmio;
    KTEST_ASSERT(vmmMapMmio(pa, 2 * 4096, &mmio) == STATUS_OK);
    uint64_t va = (uint64_t)(uintptr_t)mmio;
    KTEST_ASSERT(mmioStillMapped(va, 2 * 4096));

    MmioUnmapArgs cases[] = {
        {mmio, 4},                                           /* too small: drops page 2 */
        {(volatile void *)(uintptr_t)(va + 4096), 4},        /* interior pointer */
        {mmio, 3 * 4096},                                    /* too large: runs into the guard */
        {(volatile void *)(uintptr_t)(va + 4096), 2 * 4096}, /* shifted by a page */
        {(volatile void *)(uintptr_t)pmmHhdmBase(), 4096},   /* not in the KVA region */
    };
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, mmioUnmapTrigger, &cases[i], &info);
        KTEST_ASSERT_EQ(caught ? 0xFFu : i, 0xFFu); /* on failure, prints the uncaught case */
        KTEST_ASSERT(mmioStillMapped(va, 2 * 4096));
    }

    /* A vmalloc() area is a live KVA mapping, but WB RAM, not UC MMIO. */
    void *v = vmalloc(4096, 0);
    KTEST_ASSERT(v != NULL);
    MmioUnmapArgs vm = {(volatile void *)v, 4096};
    TrapCatchInfo info;
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, mmioUnmapTrigger, &vm, &info));
    KTEST_ASSERT(vmmLookupKernel((uint64_t)(uintptr_t)v, NULL, NULL) == STATUS_OK);
    vfree(v);

    vmmUnmapMmio(mmio, 2 * 4096);
    KTEST_ASSERT(vmmLookupKernel(va, NULL, NULL) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(vmmLookupKernel(va + 4096, NULL, NULL) == STATUS_ERR_NOT_FOUND);
    MmioUnmapArgs twice = {mmio, 2 * 4096};
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, mmioUnmapTrigger, &twice, &info));
}

/* First-fit probe: the VA vmmKvaAlloc() hands out for `len` right now (released again at once).
 * A failed vmmMapMmio() that leaked its reservation would move the next probe of the same length
 * (it takes the same lowest fitting hole the probe did), so equal probes prove the rollback. */
static uint64_t kvaProbe(uint64_t len) {
    uint64_t va = 0;
    if (vmmKvaAlloc(len, &va) == STATUS_OK) {
        vmmKvaFree(va, len);
    }
    return va;
}

/* vmmMapMmio()'s edges (D-171): a sub-page offset whose range crosses into a second page maps
 * both (bracketed by unmapped guards) and round-trips through vmmUnmapMmio(); overflow and past-
 * MAXPHYADDR ranges are rejected; every failure after vmmKvaAlloc() releases its KVA; UC is
 * refused over the framebuffer (HHDM-WC) and over RAM behind a large HHDM leaf, while the
 * existing WC/WB rules there are unchanged. */
KTEST(vmm_mmio_edges) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK && a->madt.ioapicCount >= 1);
    uint64_t pa = a->madt.ioapics[0].address;

    /* 0x20 bytes starting 0x10 before a page boundary: two pages, offset 0xFF0. */
    volatile void *mmio;
    KTEST_ASSERT(vmmMapMmio(pa + 0xFF0, 0x20, &mmio) == STATUS_OK);
    uint64_t va = (uint64_t)(uintptr_t)mmio;
    KTEST_ASSERT_EQ(va & 0xFFF, 0xFF0);
    uint64_t base = va & ~0xFFFULL;
    uint64_t outPa;
    VmmFlags f;
    KTEST_ASSERT(vmmLookupKernel(base, &outPa, &f) == STATUS_OK);
    KTEST_ASSERT_EQ(outPa, pa);
    KTEST_ASSERT_EQ(f, VMM_WRITE | VMM_CACHE_UC);
    KTEST_ASSERT(vmmLookupKernel(base + 4096, &outPa, &f) == STATUS_OK);
    KTEST_ASSERT_EQ(outPa, pa + 4096);
    KTEST_ASSERT_EQ(f, VMM_WRITE | VMM_CACHE_UC);
    KTEST_ASSERT(vmmLookupKernel(base - 4096, NULL, NULL) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(vmmLookupKernel(base + 2 * 4096, NULL, NULL) == STATUS_ERR_NOT_FOUND);
    vmmUnmapMmio(mmio, 0x20);
    KTEST_ASSERT(vmmLookupKernel(base, NULL, NULL) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(vmmLookupKernel(base + 4096, NULL, NULL) == STATUS_ERR_NOT_FOUND);

    /* Rejected before any KVA is reserved: size 0 and every wrap of pa + size (+ page round-up). */
    volatile void *bad = (volatile void *)0x1;
    KTEST_ASSERT(vmmMapMmio(pa, 0, &bad) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapMmio(UINT64_MAX, 1, &bad) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapMmio(UINT64_MAX - 4095, 1, &bad) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapMmio(1, UINT64_MAX, &bad) == STATUS_ERR_INVALID);
    KTEST_ASSERT(bad == (volatile void *)0x1); /* *outVa untouched on failure */

    /* Rejected by archMapPages() after vmmKvaAlloc(): the KVA must come back every time. */
    Page *page;
    KTEST_ASSERT(pmmAllocPages(9, 0, &page) == STATUS_OK); /* 2 MiB-aligned: a large HHDM leaf */
    uint64_t ramPa = pmmPageToPhys(page);
    KTEST_ASSERT(archPagingRawPte(pmmHhdmBase() + ramPa) == 0); /* not a 4 KiB leaf */
    struct {
        uint64_t pa, size;
    } fails[] = {
        {ramPa, 4096},                   /* RAM: HHDM-WB alias */
        {ramPa + 0x1FF000, 0x2000},      /* last RAM page of the block, then the next page */
        {UINT64_MAX - 8191, 4096},       /* page-rounds to the top page: past MAXPHYADDR */
        {1ULL << 52, 4096},              /* past any MAXPHYADDR */
        {(1ULL << 52) - 4096, 2 * 4096}, /* straddles MAXPHYADDR's hard ceiling */
    };
    for (uint32_t i = 0; i < sizeof(fails) / sizeof(fails[0]); i++) {
        uint64_t len = ((fails[i].pa & 0xFFF) + fails[i].size + 0xFFF) & ~0xFFFULL;
        uint64_t before = kvaProbe(len);
        KTEST_ASSERT(before != 0);
        Status st = vmmMapMmio(fails[i].pa, fails[i].size, &bad);
        KTEST_ASSERT_EQ(st == STATUS_ERR_INVALID ? 0xFFu : i, 0xFFu);
        KTEST_ASSERT_EQ(kvaProbe(len), before);
    }
    KTEST_ASSERT(bad == (volatile void *)0x1);

    /* RAM behind a large HHDM leaf: WB still maps, WC and UC are refused (exact-type match). */
    uint64_t kva;
    KTEST_ASSERT(vmmKvaAlloc(4096, &kva) == STATUS_OK);
    KTEST_ASSERT(vmmMapKernel(kva, ramPa, 4096, VMM_WRITE | VMM_CACHE_UC) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapKernel(kva, ramPa, 4096, VMM_WRITE | VMM_CACHE_WC) == STATUS_ERR_INVALID);
    KTEST_ASSERT(vmmMapKernel(kva, ramPa, 4096, VMM_WRITE) == STATUS_OK);
    KTEST_ASSERT(vmmLookupKernel(kva, NULL, &f) == STATUS_OK);
    KTEST_ASSERT_EQ(f, VMM_WRITE);
    KTEST_ASSERT(vmmUnmapKernel(kva, 4096) == STATUS_OK);

    /* The framebuffer (HHDM-WC): UC refused, WB refused, WC still maps and reads back as WC. */
    const BootInfo *bi = kernelBootInfo();
    if (bi->fb.phys != 0) {
        uint64_t fbPage = bi->fb.phys & ~0xFFFULL;
        uint64_t before = kvaProbe(4096);
        KTEST_ASSERT(vmmMapMmio(bi->fb.phys, 4, &bad) == STATUS_ERR_INVALID);
        KTEST_ASSERT_EQ(kvaProbe(4096), before);
        KTEST_ASSERT(vmmMapKernel(kva, fbPage, 4096, VMM_WRITE | VMM_CACHE_UC) ==
                     STATUS_ERR_INVALID);
        KTEST_ASSERT(vmmMapKernel(kva, fbPage, 4096, VMM_WRITE) == STATUS_ERR_INVALID);
        KTEST_ASSERT(vmmMapKernel(kva, fbPage, 4096, VMM_WRITE | VMM_CACHE_WC) == STATUS_OK);
        KTEST_ASSERT(vmmLookupKernel(kva, NULL, &f) == STATUS_OK);
        KTEST_ASSERT_EQ(f, VMM_WRITE | VMM_CACHE_WC);
        uint64_t raw = archPagingRawPte(kva);
        KTEST_ASSERT((raw & (1ULL << 3)) != 0); /* PWT */
        KTEST_ASSERT((raw & (1ULL << 4)) == 0); /* no PCD: index 1 (WC), not 3 (UC) */
        KTEST_ASSERT(vmmUnmapKernel(kva, 4096) == STATUS_OK);
    }
    vmmKvaFree(kva, 4096);
    pmmFreePages(page, 9);
}
