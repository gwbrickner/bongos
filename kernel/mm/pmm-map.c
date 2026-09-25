/* Pure BootInfo memory-map scan (D-079/D-080): turns the loader's `BootMemRegion[]` into the
 * `PmmMap` pmmInit() drives the rest of initialization from. No allocation, no klog, no arch
 * calls -- host-tested directly by tests/host/kernel_pmm_map_test.c. */
#include "pmm-internal.h"

/* Frames below 1 MiB are withheld from the buddy allocator entirely: reserved for the eventual
 * M3.5 SMP trampoline and as a hedge against BIOS-area memory that firmware sometimes reports
 * USABLE but isn't safe to write. */
#define PMM_LOW_MEM_LIMIT_PFN (0x100000ULL >> 12)
/* Nothing beyond the HHDM window (BootInfo.hhdmBase + this many bytes) is reachable through the
 * direct map, so the pmm can't manage it even if the firmware reports it USABLE. */
#define PMM_HHDM_LIMIT_PFN (BOOTINFO_HHDM_SIZE >> 12)
/* A free block is naturally aligned and at most order PMM_MAX_ORDER, so its buddy always lies
 * within the same PMM_SPAN_ALIGN_PFN-frame chunk -- widening every span to this alignment means a
 * buddy merge never needs to check whether the far side actually has a Page entry. */
#define PMM_SPAN_ALIGN_PFN (1ull << PMM_MAX_ORDER)

bool pmmMapTypeIsManaged(uint32_t type) {
    switch (type) {
        case BOOT_MEM_USABLE:
        case BOOT_MEM_LOADER_RECLAIM:
        case BOOT_MEM_KERNEL:
        case BOOT_MEM_INITRD:
        case BOOT_MEM_ACPI_RECLAIM:
            return true;
        default:
            return false;
    }
}

static uint64_t alignDownPfn(uint64_t pfn) {
    return pfn & ~(PMM_SPAN_ALIGN_PFN - 1);
}
static uint64_t alignUpPfn(uint64_t pfn) {
    return (pfn + PMM_SPAN_ALIGN_PFN - 1) & ~(PMM_SPAN_ALIGN_PFN - 1);
}
static uint64_t maxU64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}
static uint64_t minU64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

/* Widens [startPfn, endPfn) to PMM_SPAN_ALIGN_PFN alignment and either merges it into the last
 * span (regions arrive in ascending order, so a merge candidate is always the last entry) or
 * appends a new one. */
static Status pmmAddSpan(PmmMap *out, uint64_t startPfn, uint64_t endPfn) {
    if (startPfn >= endPfn) {
        return STATUS_OK;
    }
    startPfn = alignDownPfn(startPfn);
    endPfn = alignUpPfn(endPfn);
    if (out->spanCount > 0 && startPfn <= out->spans[out->spanCount - 1].endPfn) {
        PmmSpan *last = &out->spans[out->spanCount - 1];
        last->endPfn = maxU64(last->endPfn, endPfn);
        return STATUS_OK;
    }
    if (out->spanCount >= PMM_MAX_SPANS) {
        return STATUS_ERR_INVALID;
    }
    out->spans[out->spanCount] = (PmmSpan){.startPfn = startPfn, .endPfn = endPfn};
    out->spanCount++;
    return STATUS_OK;
}

Status pmmMapScan(const BootMemRegion *regions, uint32_t count, PmmMap *out) {
    *out = (PmmMap){0};

    for (uint32_t i = 0; i < count; i++) {
        const BootMemRegion *r = &regions[i];
        if ((r->base & 0xFFF) != 0 || (r->length & 0xFFF) != 0) {
            return STATUS_ERR_INVALID;
        }
        if (r->type < BOOT_MEM_USABLE || r->type > BOOT_MEM_FRAMEBUFFER) {
            return STATUS_ERR_INVALID;
        }
        uint64_t startPfn = r->base >> 12;
        uint64_t endPfn = (r->base + r->length) >> 12;
        out->typePages[r->type] += endPfn - startPfn;

        if (r->type == BOOT_MEM_LOADER_RECLAIM) {
            if (out->loaderReclaimCount >= PMM_MAX_RECLAIM_RANGES) {
                return STATUS_ERR_INVALID;
            }
            out->loaderReclaim[out->loaderReclaimCount] =
                (PmmReclaimRange){.physBase = r->base, .length = r->length};
            out->loaderReclaimCount++;
        }

        if (r->type == BOOT_MEM_USABLE) {
            uint64_t lowEnd = minU64(endPfn, PMM_LOW_MEM_LIMIT_PFN);
            if (startPfn < lowEnd) {
                out->lowReservedPages += lowEnd - startPfn;
            }
            uint64_t usableStart = maxU64(startPfn, PMM_LOW_MEM_LIMIT_PFN);
            uint64_t usableEnd = minU64(endPfn, PMM_HHDM_LIMIT_PFN);
            if (usableStart < usableEnd) {
                if (out->usableCount >= PMM_MAX_USABLE_RANGES) {
                    return STATUS_ERR_INVALID;
                }
                out->usable[out->usableCount] =
                    (PmmUsableRange){.startPfn = usableStart, .endPfn = usableEnd};
                out->usableCount++;
            }
            uint64_t highStart = maxU64(startPfn, PMM_HHDM_LIMIT_PFN);
            if (highStart < endPfn) {
                out->unmappedPages += endPfn - highStart;
            }
        }

        if (pmmMapTypeIsManaged(r->type)) {
            uint64_t clipEnd = minU64(endPfn, PMM_HHDM_LIMIT_PFN);
            if (startPfn < clipEnd) {
                Status st = pmmAddSpan(out, startPfn, clipEnd);
                if (st != STATUS_OK) {
                    return st;
                }
            }
        }
    }

    out->maxPfn = out->spanCount > 0 ? out->spans[out->spanCount - 1].endPfn : 0;
    return STATUS_OK;
}
