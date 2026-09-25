/* See kernel/include/arch/paging.h (ARCHITECTURE §6.3, D-086..D-090, ROADMAP M2.3): builds and
 * activates the kernel's own PML4, PAT, CR4 protections, W^X verification, and the arch backend
 * behind vmmMapKernel/vmmUnmapKernel/vmmLookupKernel. Every table page this file allocates comes
 * from pmmAllocPages() (order 0, zeroed) -- never the bump allocator early-map.c uses, which is
 * sealed by the time vmmInit() runs. */
#include "arch/paging.h"

#include "arch/early-map.h"
#include "cpu-impl.h"
#include "pte.h"

#include "bootinfo.h"
#include "klog.h"
#include "panic.h"
#include "pmm.h"
#include "sections.h"
#include "vmm.h"

#include <stdbool.h>
#include <stdint.h>

#define X86_MSR_IA32_PAT 0x277u
#define X86_MSR_EFER     0xC0000080u
#define X86_EFER_NXE_BIT (1ULL << 11)
#define X86_CR0_WP_BIT   (1ULL << 16)
#define X86_CR0_CD_BIT   (1ULL << 30)
#define X86_CR0_NW_BIT   (1ULL << 29)
#define X86_CR4_PGE_BIT  (1ULL << 7)
#define X86_CR4_UMIP_BIT (1ULL << 11)
#define X86_CR4_SMEP_BIT (1ULL << 20)
#define X86_CR4_SMAP_BIT (1ULL << 21)

/* D-087: WB at index 0/4 (unchanged from the power-on default), WC at 1/5 (was WT), UC- at 2/6
 * (unchanged -- the loader's own D-071 framebuffer-fallback mapping relies on this), UC at 3/7
 * (unchanged). Indices 4-7 mirror 0-3 so a stray PAT bit (there never is one -- this file never
 * sets it) can't change a mapping's type. */
#define X86_PAT_VALUE 0x0007010600070106ULL

#define ARCH_PML4_KERNEL_START      256u
#define ARCH_PML4_KERNEL_END        512u
#define ARCH_PML4_PAGE_ARRAY_START  448u /* VM_PAGE_ARRAY_BASE >> 39 & 511 */
#define ARCH_PML4_PAGE_ARRAY_END    480u /* VM_PAGE_ARRAY_END   >> 39 & 511 */

static uint64_t kernelPml4Phys;
static bool tablesActive = false;
static bool has1GPages = false;

static inline uint32_t pml4Index(uint64_t va) {
    return (uint32_t)((va >> 39) & 511);
}
static inline uint32_t pdptIndex(uint64_t va) {
    return (uint32_t)((va >> 30) & 511);
}
static inline uint32_t pdIndex(uint64_t va) {
    return (uint32_t)((va >> 21) & 511);
}
static inline uint32_t ptIndex(uint64_t va) {
    return (uint32_t)((va >> 12) & 511);
}

static inline uint64_t *tableAt(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(pmmHhdmBase() + (phys & X86_PTE_ADDR_MASK));
}

static Status allocTable(uint64_t *outPhys) {
    Page *page;
    Status st = pmmAllocPages(0, PMM_FLAG_ZERO, &page);
    if (st != STATUS_OK) {
        return st;
    }
    *outPhys = pmmPageToPhys(page);
    return STATUS_OK;
}

/* Returns the next-level table for `table[idx]`, allocating a fresh (zeroed, pmm-backed) one if
 * absent. A present entry with PS set is a conflict: a large leaf already sits where a subtable is
 * needed. Non-leaf entries are always P|W only (SDM Vol 3A §4.6: W is ANDed and XD is ORed down
 * the walk, so only the leaf's own flags matter) -- archPagingVerifyWx() relies on this to skip
 * walking permissions bit-by-bit down every level. */
static Status getOrAllocTable(uint64_t *table, uint32_t idx, uint64_t **outNext) {
    uint64_t entry = table[idx];
    if (entry & X86_PTE_P) {
        if (entry & X86_PTE_PS) {
            return STATUS_ERR_INVALID;
        }
        *outNext = tableAt(entry);
        return STATUS_OK;
    }
    uint64_t newPhys;
    Status st = allocTable(&newPhys);
    if (st != STATUS_OK) {
        return st;
    }
    table[idx] = (newPhys & X86_PTE_ADDR_MASK) | X86_PTE_P | X86_PTE_W;
    *outNext = tableAt(newPhys);
    return STATUS_OK;
}

/* Boot-time-only range mapper used while building the kernel PML4 (before it's active): picks the
 * largest eligible leaf size (1 GiB/2 MiB needs the CPU to support PDPE1GB for the former, natural
 * alignment, and a physical address >= 2 MiB -- SDM Vol 3A §11.11.9's fixed-range-MTRR caution
 * about mapping the first 2 MiB with anything but 4 KiB pages) and panics on any failure: there is
 * no partially-built kernel PML4 to recover from. */
static void buildMapRange(uint64_t va, uint64_t pa, uint64_t size, uint64_t leafFlags,
                          bool allowLarge) {
    if (((va | pa | size) & (X86_PTE_SIZE_4K - 1)) != 0) {
        panic("vmm: buildMapRange: misaligned va=0x%llx pa=0x%llx size=0x%llx",
              (unsigned long long)va, (unsigned long long)pa, (unsigned long long)size);
    }
    uint64_t *pml4 = tableAt(kernelPml4Phys);
    while (size > 0) {
        uint64_t *pdpt;
        if (getOrAllocTable(pml4, pml4Index(va), &pdpt) != STATUS_OK) {
            panic("vmm: out of memory building the kernel PDPT for va=0x%llx",
                  (unsigned long long)va);
        }

        if (allowLarge && has1GPages && (va & (X86_PTE_SIZE_1G - 1)) == 0 &&
            (pa & (X86_PTE_SIZE_1G - 1)) == 0 && size >= X86_PTE_SIZE_1G &&
            pa >= X86_PTE_SIZE_2M) {
            uint32_t i3 = pdptIndex(va);
            if (pdpt[i3] & X86_PTE_P) {
                panic("vmm: PDPT conflict building the kernel HHDM at va=0x%llx",
                      (unsigned long long)va);
            }
            pdpt[i3] = (pa & X86_PTE_ADDR_MASK & ~(X86_PTE_SIZE_1G - 1)) | leafFlags | X86_PTE_PS;
            va += X86_PTE_SIZE_1G;
            pa += X86_PTE_SIZE_1G;
            size -= X86_PTE_SIZE_1G;
            continue;
        }

        uint64_t *pd;
        if (getOrAllocTable(pdpt, pdptIndex(va), &pd) != STATUS_OK) {
            panic("vmm: out of memory building the kernel PD for va=0x%llx", (unsigned long long)va);
        }

        if (allowLarge && (va & (X86_PTE_SIZE_2M - 1)) == 0 && (pa & (X86_PTE_SIZE_2M - 1)) == 0 &&
            size >= X86_PTE_SIZE_2M && pa >= X86_PTE_SIZE_2M) {
            uint32_t i2 = pdIndex(va);
            if (pd[i2] & X86_PTE_P) {
                panic("vmm: PD conflict building the kernel HHDM at va=0x%llx",
                      (unsigned long long)va);
            }
            pd[i2] = (pa & X86_PTE_ADDR_MASK & ~(X86_PTE_SIZE_2M - 1)) | leafFlags | X86_PTE_PS;
            va += X86_PTE_SIZE_2M;
            pa += X86_PTE_SIZE_2M;
            size -= X86_PTE_SIZE_2M;
            continue;
        }

        uint64_t *pt;
        if (getOrAllocTable(pd, pdIndex(va), &pt) != STATUS_OK) {
            panic("vmm: out of memory building the kernel PT for va=0x%llx", (unsigned long long)va);
        }
        uint32_t i1 = ptIndex(va);
        if (pt[i1] & X86_PTE_P) {
            panic("vmm: PT conflict building the kernel image/HHDM at va=0x%llx",
                  (unsigned long long)va);
        }
        pt[i1] = (pa & X86_PTE_ADDR_MASK) | leafFlags;
        va += X86_PTE_SIZE_4K;
        pa += X86_PTE_SIZE_4K;
        size -= X86_PTE_SIZE_4K;
    }
}

/* D-086: the Page-array PML4 slots (448-479) are *adopted*, not re-mapped -- pmmInit() already
 * built that subtree entirely out of bump-allocator pages before vmmInit() ever runs. Every table
 * page under an adopted slot must still be PAGE_STATE_RESERVED bump memory (never something
 * M2.3's own pmmAllocPages() calls above have since handed out, and never a LOADER_RECLAIM page --
 * pmmInit()'s own early-map.c contract already guarantees this, but a fresh kernel PML4 is
 * exactly the kind of place a violation would silently corrupt if it were ever wrong). */
static void validateReservedTablePage(uint64_t phys) {
    Page *p = pmmPhysToPage(phys);
    if (p == NULL || p->state != PAGE_STATE_RESERVED) {
        panic("vmm: Page-array table page 0x%llx is not reserved bump-allocator memory",
              (unsigned long long)phys);
    }
}

static void adoptPageArraySubtree(uint64_t pdptPhys) {
    validateReservedTablePage(pdptPhys);
    uint64_t *pdpt = tableAt(pdptPhys);
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t e3 = pdpt[i];
        if (!(e3 & X86_PTE_P) || (e3 & X86_PTE_PS)) {
            continue;
        }
        uint64_t pdPhys = e3 & X86_PTE_ADDR_MASK;
        validateReservedTablePage(pdPhys);
        uint64_t *pd = tableAt(pdPhys);
        for (uint32_t j = 0; j < 512; j++) {
            uint64_t e2 = pd[j];
            if (!(e2 & X86_PTE_P) || (e2 & X86_PTE_PS)) {
                continue;
            }
            validateReservedTablePage(e2 & X86_PTE_ADDR_MASK);
        }
    }
}

void archPatInit(void) {
    uint32_t regs[4];
    archCpuid(1, 0, regs);
    if (!(regs[3] & (1u << 16))) { /* CPUID.01H:EDX[16] = PAT */
        panic("archPatInit: CPU does not report PAT support");
    }

    /* SDM Vol 3A §11.11.8/§11.12.4's MP-safe MSR-write procedure: disable/flush caching around the
     * WRMSR so no stale line survives under the old PAT interpretation. IF is already 0 this early
     * in boot (no IRQ source exists before M3.2), and this is the BSP alone (SMP is M3.5), so
     * there's no other CPU to race. */
    uint64_t cr0 = archReadCr0();
    uint64_t cr4 = archReadCr4();
    archWriteCr0((cr0 | X86_CR0_CD_BIT) & ~X86_CR0_NW_BIT);
    archWbinvd();
    archWriteCr4(cr4 & ~X86_CR4_PGE_BIT);
    archWrmsr(X86_MSR_IA32_PAT, X86_PAT_VALUE);
    archWbinvd();
    archWriteCr4(cr4);
    archWriteCr0(cr0);

    uint64_t readback = archRdmsr(X86_MSR_IA32_PAT);
    if (readback != X86_PAT_VALUE) {
        panic("archPatInit: IA32_PAT readback mismatch (wrote 0x%llx, read 0x%llx)",
              (unsigned long long)X86_PAT_VALUE, (unsigned long long)readback);
    }
    klogWrite(KLOG_INFO, "vmm", "IA32_PAT=0x%016llx (WB/WC/UC-/UC)",
              (unsigned long long)X86_PAT_VALUE);
}

void archPagingBuildKernel(const BootInfo *bi, const BootMemRegion *map, uint32_t mapCount) {
    if (kernelPml4Phys != 0) {
        panic("archPagingBuildKernel: called twice");
    }

    uint32_t regs[4];
    archCpuid(0x80000001u, 0, regs);
    has1GPages = (regs[3] & (1u << 26)) != 0; /* CPUID.80000001H:EDX[26] = PDPE1GB */

    /* Confirms the physOf(va) arithmetic every mapping below relies on, against the loader's own
     * (still live at this point) tables -- a KASLR-slide or kernelPhysBase bug here would
     * otherwise map the whole kernel image at the wrong physical address and only surface much
     * later as a baffling fault. */
    uint64_t checkPa, checkLeafSize;
    Status lst = archEarlyLookup((uint64_t)(uintptr_t)kernelTextStart, &checkPa, &checkLeafSize);
    if (lst != STATUS_OK || checkPa != bi->kernelPhysBase) {
        panic("vmm: kernelTextStart does not resolve to kernelPhysBase under the loader's tables");
    }

    uint64_t pml4Phys;
    if (allocTable(&pml4Phys) != STATUS_OK) {
        panic("vmm: out of memory allocating the kernel PML4");
    }
    kernelPml4Phys = pml4Phys;

    uint64_t *loaderPml4 = tableAt(archReadCr3() & X86_PTE_ADDR_MASK);
    uint64_t *kpml4 = tableAt(kernelPml4Phys);
    for (uint32_t idx = ARCH_PML4_KERNEL_START; idx < ARCH_PML4_KERNEL_END; idx++) {
        if (idx >= ARCH_PML4_PAGE_ARRAY_START && idx < ARCH_PML4_PAGE_ARRAY_END &&
            (loaderPml4[idx] & X86_PTE_P)) {
            uint64_t entry = loaderPml4[idx];
            if (entry & X86_PTE_PS) {
                panic("vmm: unexpected huge PML4 entry in the Page-array VA range");
            }
            adoptPageArraySubtree(entry & X86_PTE_ADDR_MASK);
            kpml4[idx] = entry;
            continue;
        }
        uint64_t pdptPhys;
        if (allocTable(&pdptPhys) != STATUS_OK) {
            panic("vmm: out of memory allocating kernel-half PML4 slot %u", idx);
        }
        kpml4[idx] = (pdptPhys & X86_PTE_ADDR_MASK) | X86_PTE_P | X86_PTE_W;
    }

    /* HHDM: one mapping per BootInfo region. Regions of the same type never touch (the
     * memmap_coalesced ktest's own invariant on the loader's map), so no merge pass is needed
     * here. RESERVED/BAD are deliberately left unmapped -- the HHDM narrows starting in M2.3
     * (ARCHITECTURE §5.4/§6.1). */
    uint64_t textPhysStart = bi->kernelPhysBase;
    uint64_t roPhysEnd = bi->kernelPhysBase + ((uint64_t)(uintptr_t)kernelRodataEnd -
                                               (uint64_t)(uintptr_t)kernelImageStart);
    for (uint32_t i = 0; i < mapCount; i++) {
        const BootMemRegion *r = &map[i];
        bool wc = false;
        switch (r->type) {
            case BOOT_MEM_USABLE:
            case BOOT_MEM_LOADER_RECLAIM:
            case BOOT_MEM_KERNEL:
            case BOOT_MEM_INITRD:
            case BOOT_MEM_ACPI_RECLAIM:
            case BOOT_MEM_ACPI_NVS:
                break;
            case BOOT_MEM_FRAMEBUFFER:
                wc = true;
                break;
            default:
                continue;
        }
        uint64_t base = r->base;
        uint64_t end = r->base + r->length;
        if (base >= BOOTINFO_HHDM_SIZE) {
            continue;
        }
        if (end > BOOTINFO_HHDM_SIZE) {
            end = BOOTINFO_HHDM_SIZE;
        }
        if (base >= end) {
            continue;
        }

        if (r->type == BOOT_MEM_KERNEL) {
            /* Carve the text+rodata physical range out read-only (D-090 point g): its HHDM alias
             * must never be writable, or a writer through the direct map could modify "read-only"
             * kernel code/data despite the image-address mapping refusing to. */
            uint64_t roStart = base > textPhysStart ? base : textPhysStart;
            uint64_t roEnd = end < roPhysEnd ? end : roPhysEnd;
            if (base < roStart) {
                buildMapRange(BOOTINFO_HHDM_BASE + base, base, roStart - base,
                              X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G, true);
            }
            if (roStart < roEnd) {
                buildMapRange(BOOTINFO_HHDM_BASE + roStart, roStart, roEnd - roStart,
                              X86_PTE_P | X86_PTE_NX | X86_PTE_G, true);
            }
            if (roEnd < end) {
                buildMapRange(BOOTINFO_HHDM_BASE + roEnd, roEnd, end - roEnd,
                              X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G, true);
            }
            continue;
        }

        uint64_t flags = X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G | (wc ? X86_PTE_PWT : 0);
        buildMapRange(BOOTINFO_HHDM_BASE + base, base, end - base, flags, !wc);
    }
    if (bi->fb.phys != 0) {
        klogWrite(KLOG_INFO, "vmm", "framebuffer 0x%llx mapped WC in the HHDM",
                  (unsigned long long)bi->fb.phys);
    }

    /* The kernel image itself, at its link-time VAs -- 4 KiB leaves only (D-086), so a later
     * vmmUnmapKernel-style change to one page never has to split a large leaf. */
    struct {
        const uint8_t *start, *end;
        uint64_t flags;
    } segs[] = {
        {kernelTextStart, kernelTextEnd, X86_PTE_P | X86_PTE_G}, /* R-X: no W, no NX */
        {kernelRodataStart, kernelRodataEnd, X86_PTE_P | X86_PTE_NX | X86_PTE_G},
        {kernelDataStart, kernelDataEnd, X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G},
        {kernelBootStackBottom, kernelBootStackTop, X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G},
        {kernelIst1Bottom, kernelIst1Top, X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G},
        {kernelIst2Bottom, kernelIst2Top, X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G},
        {kernelIst3Bottom, kernelIst3Top, X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G},
    };
    for (uint32_t i = 0; i < sizeof(segs) / sizeof(segs[0]); i++) {
        if (segs[i].end == segs[i].start) {
            continue;
        }
        uint64_t va = (uint64_t)(uintptr_t)segs[i].start;
        uint64_t size = (uint64_t)(segs[i].end - segs[i].start);
        uint64_t pa = bi->kernelPhysBase + (va - bi->kernelVirtBase);
        buildMapRange(va, pa, size, segs[i].flags, false);
    }
}

void archPagingActivate(void) {
    if (kernelPml4Phys == 0) {
        panic("archPagingActivate: archPagingBuildKernel was never called");
    }
    uint64_t cr4 = archReadCr4();
    /* PGE 1->0 flushes every TLB/paging-structure-cache entry, including the loader's own global
     * HHDM/image/framebuffer entries (SDM Vol 3A §4.10.4.1) -- without this, a stale global
     * translation from the loader's tables (e.g. its writable HHDM alias of kernel text, or its
     * UC- framebuffer entry) could still be used after CR3 changes underneath it. The CR3 write
     * itself is serializing, so every table store archPagingBuildKernel() made is guaranteed
     * visible to the page walker before it runs. PGE 0->1 flushes once more so the new mappings'
     * own G bit takes effect cleanly rather than inheriting stale non-global entries. */
    archWriteCr4(cr4 & ~X86_CR4_PGE_BIT);
    archWriteCr3(kernelPml4Phys);
    archWriteCr4(cr4);
    tablesActive = true;
}

void archCpuEnableProtections(void) {
    if (!tablesActive) {
        panic("archCpuEnableProtections: called before archPagingActivate()");
    }
    if (!(archReadCr0() & X86_CR0_WP_BIT)) {
        panic("archCpuEnableProtections: CR0.WP is not set (ARCHITECTURE §5.4 loader contract)");
    }
    if (!(archRdmsr(X86_MSR_EFER) & X86_EFER_NXE_BIT)) {
        panic("archCpuEnableProtections: EFER.NXE is not set (ARCHITECTURE §5.4 loader contract)");
    }

    uint64_t cr4 = archReadCr4();
    uint32_t leaf0[4];
    archCpuid(0, 0, leaf0);
    if (leaf0[0] >= 7) {
        uint32_t leaf7[4];
        archCpuid(7, 0, leaf7);
        if (leaf7[1] & (1u << 7)) { /* EBX[7] SMEP */
            cr4 |= X86_CR4_SMEP_BIT;
        }
        if (leaf7[1] & (1u << 20)) { /* EBX[20] SMAP */
            cr4 |= X86_CR4_SMAP_BIT;
        }
        if (leaf7[2] & (1u << 2)) { /* ECX[2] UMIP */
            cr4 |= X86_CR4_UMIP_BIT;
        }
    }
    archWriteCr4(cr4);
    klogWrite(KLOG_INFO, "vmm", "cpu protections: SMEP=%s SMAP=%s UMIP=%s",
              (cr4 & X86_CR4_SMEP_BIT) ? "on" : "off", (cr4 & X86_CR4_SMAP_BIT) ? "on" : "off",
              (cr4 & X86_CR4_UMIP_BIT) ? "on" : "off");
}

static uint64_t *findLeafPte(uint64_t va) {
    uint64_t *pml4 = tableAt(kernelPml4Phys);
    uint64_t e4 = pml4[pml4Index(va)];
    if (!(e4 & X86_PTE_P)) {
        return NULL;
    }
    uint64_t *pdpt = tableAt(e4);
    uint64_t e3 = pdpt[pdptIndex(va)];
    if (!(e3 & X86_PTE_P) || (e3 & X86_PTE_PS)) {
        return NULL; /* not present, or covered by a large leaf -- callers only ever target 4 KiB-
                      * mapped VAs (the KVA region, or the framebuffer's always-4K HHDM leaf) */
    }
    uint64_t *pd = tableAt(e3);
    uint64_t e2 = pd[pdIndex(va)];
    if (!(e2 & X86_PTE_P) || (e2 & X86_PTE_PS)) {
        return NULL;
    }
    uint64_t *pt = tableAt(e2);
    return &pt[ptIndex(va)];
}

static void checkLeaf(uint64_t va, uint64_t entry, uint64_t leafSize, uint64_t textStart,
                      uint64_t textEnd, uint64_t *leafCount, uint64_t *execLeafCount) {
    if (!(entry & X86_PTE_G)) {
        panic("vmm: W^X verify: leaf at 0x%llx is not global", (unsigned long long)va);
    }
    bool w = (entry & X86_PTE_W) != 0;
    bool x = (entry & X86_PTE_NX) == 0;
    if (w && x) {
        panic("vmm: W^X verify: leaf at 0x%llx is writable AND executable",
              (unsigned long long)va);
    }
    (*leafCount)++;
    if (!x) {
        return;
    }
    if (leafSize != X86_PTE_SIZE_4K || va < textStart || va + leafSize > textEnd) {
        panic("vmm: W^X verify: executable leaf at 0x%llx lies outside kernel text",
              (unsigned long long)va);
    }
    (*execLeafCount)++;
    /* D-090 point (g): the HHDM alias of this same physical frame must never be writable, or a
     * write through the direct map could modify supposedly-read-only executable code. */
    uint64_t pa = entry & X86_PTE_ADDR_MASK;
    uint64_t *hhdmPte = findLeafPte(BOOTINFO_HHDM_BASE + pa);
    if (hhdmPte != NULL && (*hhdmPte & X86_PTE_P) && (*hhdmPte & X86_PTE_W)) {
        panic("vmm: W^X verify: HHDM alias of text frame 0x%llx is writable",
              (unsigned long long)pa);
    }
}

void archPagingVerifyWx(void) {
    if (!tablesActive) {
        panic("archPagingVerifyWx: called before archPagingActivate()");
    }
    if (!(archReadCr0() & X86_CR0_WP_BIT)) {
        panic("vmm: W^X verify: CR0.WP is not set");
    }
    if (!(archRdmsr(X86_MSR_EFER) & X86_EFER_NXE_BIT)) {
        panic("vmm: W^X verify: EFER.NXE is not set");
    }

    uint64_t *pml4 = tableAt(kernelPml4Phys);
    for (uint32_t i = 0; i < ARCH_PML4_KERNEL_START; i++) {
        if (pml4[i] & X86_PTE_P) {
            panic("vmm: W^X verify: lower-half PML4[%u] is present", i);
        }
    }

    uint64_t leafCount = 0, execLeafCount = 0;
    uint64_t textStart = (uint64_t)(uintptr_t)kernelTextStart;
    uint64_t textEnd = (uint64_t)(uintptr_t)kernelTextEnd;

    for (uint32_t i4 = ARCH_PML4_KERNEL_START; i4 < ARCH_PML4_KERNEL_END; i4++) {
        uint64_t e4 = pml4[i4];
        if (!(e4 & X86_PTE_P)) {
            panic("vmm: W^X verify: kernel-half PML4[%u] is not present", i4);
        }
        if (e4 & X86_PTE_US) {
            panic("vmm: W^X verify: PML4[%u] has U/S set", i4);
        }
        /* Canonical-address sign extension (SDM Vol 1 §3.3.7.1): every kernel-half index has bit
         * 47 set, so bits 63:48 must be forced to 1 too, or the reconstructed VA below is a
         * non-canonical (and simply wrong) address instead of the real higher-half one. */
        uint64_t va4 = 0xFFFF000000000000ULL | ((uint64_t)i4 << 39);
        uint64_t *pdpt = tableAt(e4);
        for (uint32_t i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            if (!(e3 & X86_PTE_P)) {
                continue;
            }
            if (e3 & X86_PTE_US) {
                panic("vmm: W^X verify: PDPT entry has U/S set");
            }
            uint64_t va3 = va4 | ((uint64_t)i3 << 30);
            if (e3 & X86_PTE_PS) {
                checkLeaf(va3, e3, X86_PTE_SIZE_1G, textStart, textEnd, &leafCount, &execLeafCount);
                continue;
            }
            uint64_t *pd = tableAt(e3);
            for (uint32_t i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                if (!(e2 & X86_PTE_P)) {
                    continue;
                }
                if (e2 & X86_PTE_US) {
                    panic("vmm: W^X verify: PD entry has U/S set");
                }
                uint64_t va2 = va3 | ((uint64_t)i2 << 21);
                if (e2 & X86_PTE_PS) {
                    checkLeaf(va2, e2, X86_PTE_SIZE_2M, textStart, textEnd, &leafCount,
                             &execLeafCount);
                    continue;
                }
                uint64_t *pt = tableAt(e2);
                for (uint32_t i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = pt[i1];
                    if (!(e1 & X86_PTE_P)) {
                        continue;
                    }
                    if (e1 & X86_PTE_US) {
                        panic("vmm: W^X verify: PT entry has U/S set");
                    }
                    uint64_t va1 = va2 | ((uint64_t)i1 << 12);
                    checkLeaf(va1, e1, X86_PTE_SIZE_4K, textStart, textEnd, &leafCount,
                             &execLeafCount);
                }
            }
        }
    }

    klogWrite(KLOG_INFO, "vmm",
              "W^X verified: %llu leaves, %llu executable (kernel text only), "
              "0 writable+executable, lower half empty",
              (unsigned long long)leafCount, (unsigned long long)execLeafCount);
}

static void clearLeaf(uint64_t va) {
    uint64_t *pte = findLeafPte(va);
    *pte = 0;
    archInvlpg(va);
}

Status archMapPages(uint64_t va, uint64_t pa, uint64_t size, VmmFlags flags) {
    if (!tablesActive) {
        panic("archMapPages: called before archPagingActivate()");
    }
    if (((va | pa | size) & (X86_PTE_SIZE_4K - 1)) != 0 || size == 0) {
        return STATUS_ERR_INVALID;
    }
    if (flags & VMM_EXEC) {
        return STATUS_ERR_UNSUPPORTED;
    }
    if ((flags & ~(VmmFlags)VMM_FLAGS_VALID) != 0) {
        return STATUS_ERR_INVALID;
    }

    bool wc = (flags & VMM_CACHE_MASK) == VMM_CACHE_WC;
    /* Anti-aliasing (SDM Vol 3A §11.12.4, D-088): a WC request over a physical page the pmm
     * already manages (and therefore already maps WB through the HHDM) would create two
     * incompatible-type mappings of the same physical page. */
    if (wc) {
        for (uint64_t off = 0; off < size; off += X86_PTE_SIZE_4K) {
            if (pmmPhysToPage(pa + off) != NULL) {
                return STATUS_ERR_INVALID;
            }
        }
    }

    uint64_t leafFlags = X86_PTE_P | X86_PTE_NX | X86_PTE_G;
    if (flags & VMM_WRITE) {
        leafFlags |= X86_PTE_W;
    }
    if (wc) {
        leafFlags |= X86_PTE_PWT;
    }

    uint64_t *pml4 = tableAt(kernelPml4Phys);
    uint64_t mapped = 0;
    Status st = STATUS_OK;
    for (uint64_t off = 0; off < size; off += X86_PTE_SIZE_4K) {
        uint64_t v = va + off;
        uint64_t *pdpt, *pd, *pt;
        st = getOrAllocTable(pml4, pml4Index(v), &pdpt);
        if (st == STATUS_OK) {
            st = getOrAllocTable(pdpt, pdptIndex(v), &pd);
        }
        if (st == STATUS_OK) {
            st = getOrAllocTable(pd, pdIndex(v), &pt);
        }
        if (st != STATUS_OK) {
            break;
        }
        uint32_t i1 = ptIndex(v);
        if (pt[i1] & X86_PTE_P) {
            st = STATUS_ERR_INVALID;
            break;
        }
        pt[i1] = ((pa + off) & X86_PTE_ADDR_MASK) | leafFlags;
        mapped += X86_PTE_SIZE_4K;
    }
    if (st != STATUS_OK) {
        for (uint64_t off = 0; off < mapped; off += X86_PTE_SIZE_4K) {
            clearLeaf(va + off);
        }
        return st;
    }
    return STATUS_OK;
}

Status archUnmapPages(uint64_t va, uint64_t size) {
    if (!tablesActive) {
        panic("archUnmapPages: called before archPagingActivate()");
    }
    if (((va | size) & (X86_PTE_SIZE_4K - 1)) != 0 || size == 0) {
        return STATUS_ERR_INVALID;
    }
    for (uint64_t off = 0; off < size; off += X86_PTE_SIZE_4K) {
        uint64_t *pte = findLeafPte(va + off);
        if (pte == NULL || !(*pte & X86_PTE_P)) {
            return STATUS_ERR_NOT_FOUND;
        }
    }
    for (uint64_t off = 0; off < size; off += X86_PTE_SIZE_4K) {
        clearLeaf(va + off);
    }
    return STATUS_OK;
}

Status archLookupKernel(uint64_t va, uint64_t *outPa, VmmFlags *outFlags) {
    if (!tablesActive) {
        panic("archLookupKernel: called before archPagingActivate()");
    }
    if ((va & (X86_PTE_SIZE_4K - 1)) != 0) {
        return STATUS_ERR_INVALID;
    }
    uint64_t *pte = findLeafPte(va);
    if (pte == NULL || !(*pte & X86_PTE_P)) {
        return STATUS_ERR_NOT_FOUND;
    }
    uint64_t e = *pte;
    if (outPa != NULL) {
        *outPa = e & X86_PTE_ADDR_MASK;
    }
    if (outFlags != NULL) {
        VmmFlags f = 0;
        if (e & X86_PTE_W) {
            f |= VMM_WRITE;
        }
        if (!(e & X86_PTE_NX)) {
            f |= VMM_EXEC;
        }
        if (e & X86_PTE_PWT) {
            f |= VMM_CACHE_WC;
        }
        *outFlags = f;
    }
    return STATUS_OK;
}

void archTlbInvalidateKernelRange(uint64_t va, uint64_t size) {
    for (uint64_t off = 0; off < size; off += X86_PTE_SIZE_4K) {
        archInvlpg(va + off);
    }
}

uint64_t archPagingRawPte(uint64_t va) {
    if (!tablesActive) {
        return 0;
    }
    uint64_t *pte = findLeafPte(va);
    return pte != NULL ? *pte : 0;
}
