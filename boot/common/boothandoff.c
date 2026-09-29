/* See boothandoff.h. */
#include "include/boothandoff.h"

#include "include/bootmem.h"

#define HANDOFF_PAGE_SIZE 4096ULL

BootStatus bootAllocAdd(BootAllocList *list, uint64_t base, uint64_t pages, uint32_t type) {
    if (list->count >= BOOT_HANDOFF_MAX_ALLOCS) {
        return BOOT_ERR_MEMMAP_CAPACITY;
    }
    list->allocs[list->count].base = base;
    list->allocs[list->count].pages = pages;
    list->allocs[list->count].type = type;
    list->count++;
    return BOOT_OK;
}

/* D-068: the one exception to D-059's "MMIO is never HHDM-mapped" rule. Zeroes `*fb` and sets
 * `*fbNote` on any problem instead of failing the whole boot -- a missing framebuffer is
 * recoverable, a corrupt one silently overlapping RAM would not be. */
static BootStatus mapFramebuffer(PtBuilder *pt, const BootPtPlan *plan, BootFramebuffer *fb,
                                 BootAllocList *allocs, const char **fbNote) {
    *fbNote = NULL;
    if (fb->phys == 0) {
        return BOOT_OK;
    }
    if (!plan->patEntry2Uncacheable) {
        *fbNote = "firmware's IA32_PAT entry 2 isn't UC/UC-";
        bootMemset(fb, 0, sizeof(*fb));
        return BOOT_OK;
    }
    uint64_t fbBase = bootAlignDown(fb->phys, HANDOFF_PAGE_SIZE);
    uint64_t fbEndUnaligned = fb->phys + (uint64_t)fb->pitch * (uint64_t)fb->height;
    uint64_t fbEnd = bootAlignUp(fbEndUnaligned, HANDOFF_PAGE_SIZE);
    if (fbEnd < fbEndUnaligned /* overflow */ || fbEnd <= fbBase) {
        *fbNote = "invalid geometry";
        bootMemset(fb, 0, sizeof(*fb));
        return BOOT_OK;
    }
    uint64_t fbSize = fbEnd - fbBase;
    if (fbBase >= BOOTINFO_HHDM_SIZE || fbSize > BOOTINFO_HHDM_SIZE - fbBase) {
        *fbNote = "beyond the HHDM window";
        bootMemset(fb, 0, sizeof(*fb));
        return BOOT_OK;
    }

    uint64_t checkPa, checkFlags;
    bool alreadyMapped = false;
    for (uint64_t va = BOOTINFO_HHDM_BASE + fbBase; va < BOOTINFO_HHDM_BASE + fbEnd;
         va += HANDOFF_PAGE_SIZE) {
        if (ptLookup(pt, va, &checkPa, &checkFlags) == BOOT_OK) {
            alreadyMapped = true;
            break;
        }
    }
    if (alreadyMapped) {
        *fbNote = "overlaps an existing mapping";
        bootMemset(fb, 0, sizeof(*fb));
        return BOOT_OK;
    }

    BootStatus bst =
        ptMapRange(pt, BOOTINFO_HHDM_BASE + fbBase, fbBase, fbSize, PT_FLAGS_FRAMEBUFFER, false);
    if (bst != BOOT_OK) {
        /* ptMapRange() never partially undoes a failed range -- but every page in [fbBase, fbEnd)
         * was just confirmed unmapped above, and a failure here can only be BOOT_ERR_NO_MEMORY
         * (the page-table pool exhausted), since conflict/alignment are already ruled out. Either
         * way there is nothing to roll back, which is exactly why `fb` is zeroed and no overlay is
         * recorded rather than trusting a partial mapping. */
        *fbNote = bootStatusString(bst);
        bootMemset(fb, 0, sizeof(*fb));
        return BOOT_OK;
    }
    return bootAllocAdd(allocs, fbBase, fbSize / HANDOFF_PAGE_SIZE, BOOT_MEM_FRAMEBUFFER);
}

BootStatus bootHandoffMapAll(PtBuilder *pt, const BootPtPlan *plan, BootFramebuffer *fb,
                             BootAllocList *allocs, const char **fbNote) {
    for (uint32_t i = 0; i < plan->hhdmRunCount; i++) {
        uint64_t base = plan->hhdmRuns[i].base;
        uint64_t length = plan->hhdmRuns[i].length;
        if (base >= BOOTINFO_HHDM_SIZE) {
            continue; /* clipped: beyond the 64 TiB HHDM window */
        }
        if (length > BOOTINFO_HHDM_SIZE - base) {
            length = BOOTINFO_HHDM_SIZE - base;
        }
        BootStatus bst =
            ptMapRange(pt, BOOTINFO_HHDM_BASE + base, base, length, PT_FLAGS_HHDM, true);
        if (bst != BOOT_OK) {
            return bst;
        }
    }
    BootStatus bst = ptMapElfImage(pt, plan->elfImage, plan->kernelPhys, 0);
    if (bst != BOOT_OK) {
        return bst;
    }
    bst = ptMapRange(pt, plan->trampPhys, plan->trampPhys, HANDOFF_PAGE_SIZE, PT_FLAGS_TRAMPOLINE,
                     false);
    if (bst != BOOT_OK) {
        return bst;
    }
    return mapFramebuffer(pt, plan, fb, allocs, fbNote);
}

bool bootHandoffSelfCheck(const PtBuilder *pt, uint64_t entryVa, uint64_t stackTopVa,
                          uint64_t bootInfoVa, uint64_t cmdlineVa, uint64_t memMapVa,
                          uint64_t trampPhys, const BootFramebuffer *fb) {
    uint64_t checkPa, checkFlags;
    bool ok = ptLookup(pt, entryVa, &checkPa, &checkFlags) == BOOT_OK && (checkFlags & PT_W) == 0 &&
              (checkFlags & PT_NX) == 0 &&
              ptLookup(pt, stackTopVa - 8, &checkPa, &checkFlags) == BOOT_OK &&
              (checkFlags & PT_W) != 0 && (checkFlags & PT_NX) != 0 &&
              ptLookup(pt, bootInfoVa, &checkPa, &checkFlags) == BOOT_OK &&
              /* Spot-check the rest of the handoff block too, not just page 0 (BootInfo): the
               * cmdline page and (the start of) the memory-map array are just as load-bearing for
               * the kernel's first instructions. */
              ptLookup(pt, cmdlineVa, &checkPa, &checkFlags) == BOOT_OK &&
              ptLookup(pt, memMapVa, &checkPa, &checkFlags) == BOOT_OK &&
              ptLookup(pt, trampPhys, &checkPa, &checkFlags) == BOOT_OK &&
              (checkFlags & PT_NX) == 0;
    if (ok && fb->phys != 0) {
        /* Check both ends of the mapped range, not just the first page: a partial-mapping bug
         * could leave the tail pages missing PT_W/PT_PCD/PT_NX (or absent) while the head looks
         * fine. checkPa == fbBase confirms the VA->PA translation itself, not just its flags. */
        uint64_t fbBase = bootAlignDown(fb->phys, HANDOFF_PAGE_SIZE);
        uint64_t fbEndUnaligned = fb->phys + (uint64_t)fb->pitch * (uint64_t)fb->height;
        uint64_t fbEnd = bootAlignUp(fbEndUnaligned, HANDOFF_PAGE_SIZE);
        uint64_t fbFirstVa = BOOTINFO_HHDM_BASE + fbBase;
        uint64_t fbLastVa = BOOTINFO_HHDM_BASE + fbEnd - HANDOFF_PAGE_SIZE;
        ok = ptLookup(pt, fbFirstVa, &checkPa, &checkFlags) == BOOT_OK && checkPa == fbBase &&
             (checkFlags & PT_PCD) != 0 && (checkFlags & PT_NX) != 0 && (checkFlags & PT_W) != 0 &&
             ptLookup(pt, fbLastVa, &checkPa, &checkFlags) == BOOT_OK &&
             (checkFlags & PT_PCD) != 0 && (checkFlags & PT_NX) != 0 && (checkFlags & PT_W) != 0;
    }
    return ok;
}

void bootHandoffFillInfo(BootInfo *bi, const BootHandoffFields *fields) {
    bootMemset(bi, 0, sizeof(*bi));
    bi->magic = BOOTINFO_MAGIC;
    bi->version = BOOTINFO_VERSION;
    bi->size = sizeof(BootInfo);
    bi->bootMethod = fields->bootMethod;
    bi->fb = fields->fb;
    bi->rsdpPhys = fields->rsdpPhys;
    bi->kernelPhysBase = fields->kernelPhys;
    bi->kernelVirtBase = fields->kernelVirtBase;
    bi->kernelSize = fields->kernelSize;
    bi->kaslrSlide = 0;
    bi->cmdlinePhys = fields->cmdlinePhys;
    bi->hhdmBase = fields->hhdmBase;
    bi->loaderTsc = fields->loaderTsc;
    bi->efiSystemTablePhys = fields->efiSystemTablePhys;
    bootMemcpy(bi->randomSeed, fields->randomSeed, sizeof(bi->randomSeed));
}

BootStatus bootHandoffFinalMap(const MemMapInput *fwInputs, uint32_t nFwInputs,
                               const BootAllocList *allocs, MemMapInput *work, uint32_t workCap,
                               uint64_t *scratch, BootMemRegion *out, uint32_t outCap,
                               uint32_t *nOut) {
    if ((uint64_t)nFwInputs + allocs->count > workCap) {
        return BOOT_ERR_MEMMAP_CAPACITY;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < nFwInputs; i++) {
        work[n++] = fwInputs[i];
    }
    for (uint32_t i = 0; i < allocs->count; i++) {
        work[n].base = allocs->allocs[i].base;
        work[n].length = allocs->allocs[i].pages * HANDOFF_PAGE_SIZE;
        work[n].type = allocs->allocs[i].type;
        n++;
    }
    return memMapNormalize(work, n, out, outCap, nOut, scratch);
}
