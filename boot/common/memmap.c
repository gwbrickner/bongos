/* See memmap.h. */
#include "include/memmap.h"

#include "include/bootmem.h"

#define MEM_MAP_PAGE_SIZE 4096ULL

uint32_t memMapEfiTypeToBootMem(uint32_t efiType, uint64_t attribute) {
    switch (efiType) {
        case 0: /* EfiReservedMemoryType */
            return BOOT_MEM_RESERVED;
        case 1: /* EfiLoaderCode */
        case 2: /* EfiLoaderData */
        case 3: /* EfiBootServicesCode */
        case 4: /* EfiBootServicesData */
            return BOOT_MEM_USABLE;
        case 5: /* EfiRuntimeServicesCode */
        case 6: /* EfiRuntimeServicesData */
            return BOOT_MEM_RESERVED;
        case 7: /* EfiConventionalMemory */
            return (attribute & MEM_MAP_EFI_ATTRIBUTE_SP) ? BOOT_MEM_RESERVED : BOOT_MEM_USABLE;
        case 8: /* EfiUnusableMemory */
            return BOOT_MEM_BAD;
        case 9: /* EfiACPIReclaimMemory */
            return BOOT_MEM_ACPI_RECLAIM;
        case 10: /* EfiACPIMemoryNVS */
            return BOOT_MEM_ACPI_NVS;
        case 11: /* EfiMemoryMappedIO */
        case 12: /* EfiMemoryMappedIOPortSpace */
        case 13: /* EfiPalCode */
        case 14: /* EfiPersistentMemory */
        case 15: /* EfiUnacceptedMemoryType */
        default: /* any later/OEM/OS type: unknown, so treat conservatively as RESERVED */
            return BOOT_MEM_RESERVED;
    }
}

/* Rounds one raw input's [base, base+length) to a page-aligned [outBase, outEnd), per D-060: a
 * USABLE input rounds inward (base up, end down -- conservative: never claims a partial page as
 * usable), every other type rounds outward (base down, end up -- conservative: never shrinks a
 * reservation). Returns 0 (drop the input) on a zero/negative result or an overflow. */
static int memMapRoundInput(const MemMapInput *r, uint64_t *outBase, uint64_t *outEnd) {
    if (r->length == 0) {
        return 0;
    }
    uint64_t end = r->base + r->length;
    if (end < r->base) {
        return 0; /* overflow */
    }
    uint64_t base, roundedEnd;
    if (r->type == BOOT_MEM_USABLE) {
        base = bootAlignUp(r->base, MEM_MAP_PAGE_SIZE);
        roundedEnd = bootAlignDown(end, MEM_MAP_PAGE_SIZE);
    } else {
        base = bootAlignDown(r->base, MEM_MAP_PAGE_SIZE);
        roundedEnd = bootAlignUp(end, MEM_MAP_PAGE_SIZE);
        if (roundedEnd < end) {
            return 0; /* overflow rounding up */
        }
    }
    if (roundedEnd <= base) {
        return 0;
    }
    *outBase = base;
    *outEnd = roundedEnd;
    return 1;
}

static int memMapRank(uint32_t type) {
    switch (type) {
        case BOOT_MEM_USABLE:
            return 0;
        case BOOT_MEM_LOADER_RECLAIM:
            return 1;
        case BOOT_MEM_INITRD:
            return 2;
        case BOOT_MEM_KERNEL:
            return 3;
        case BOOT_MEM_ACPI_RECLAIM:
            return 4;
        case BOOT_MEM_ACPI_NVS:
            return 5;
        case BOOT_MEM_RESERVED:
            return 6;
        case BOOT_MEM_FRAMEBUFFER:
            return 7;
        case BOOT_MEM_BAD:
            return 8;
        default:
            return -1;
    }
}

BootStatus memMapNormalize(const MemMapInput *in, uint32_t nIn, BootMemRegion *out, uint32_t outCap,
                           uint32_t *nOut, uint64_t *scratchPoints) {
    uint32_t nPoints = 0;
    for (uint32_t i = 0; i < nIn; i++) {
        uint64_t base, end;
        if (!memMapRoundInput(&in[i], &base, &end)) {
            continue;
        }
        scratchPoints[nPoints++] = base;
        scratchPoints[nPoints++] = end;
    }

    /* Insertion sort: nPoints is at most 2*nIn, which stays small (a few hundred at most) even
     * for a real EFI map, so O(n^2) is fine at boot time and needs no extra scratch space. */
    for (uint32_t i = 1; i < nPoints; i++) {
        uint64_t key = scratchPoints[i];
        uint32_t j = i;
        while (j > 0 && scratchPoints[j - 1] > key) {
            scratchPoints[j] = scratchPoints[j - 1];
            j--;
        }
        scratchPoints[j] = key;
    }

    uint32_t nUnique = 0;
    for (uint32_t i = 0; i < nPoints; i++) {
        if (nUnique == 0 || scratchPoints[i] != scratchPoints[nUnique - 1]) {
            scratchPoints[nUnique++] = scratchPoints[i];
        }
    }

    *nOut = 0;
    for (uint32_t k = 0; k + 1 < nUnique; k++) {
        uint64_t segStart = scratchPoints[k];
        uint64_t segEnd = scratchPoints[k + 1];

        int bestRank = -1;
        uint32_t bestType = 0;
        for (uint32_t i = 0; i < nIn; i++) {
            uint64_t base, end;
            if (!memMapRoundInput(&in[i], &base, &end)) {
                continue;
            }
            if (base <= segStart && segEnd <= end) {
                int rank = memMapRank(in[i].type);
                if (rank > bestRank) {
                    bestRank = rank;
                    bestType = in[i].type;
                }
            }
        }
        if (bestRank < 0) {
            continue; /* a hole: nothing covers this interval */
        }

        if (*nOut > 0 && out[*nOut - 1].type == bestType &&
            out[*nOut - 1].base + out[*nOut - 1].length == segStart) {
            out[*nOut - 1].length += segEnd - segStart;
        } else {
            if (*nOut >= outCap) {
                return BOOT_ERR_MEMMAP_CAPACITY;
            }
            out[*nOut].base = segStart;
            out[*nOut].length = segEnd - segStart;
            out[*nOut].type = bestType;
            out[*nOut].reserved = 0;
            (*nOut)++;
        }
    }
    return BOOT_OK;
}

BootStatus memMapCheckOverlay(uint64_t base, uint64_t length, const MemMapInput *efiRegions,
                              uint32_t nEfi) {
    if (length == 0) {
        return BOOT_OK;
    }
    uint64_t end = base + length;
    for (uint32_t i = 0; i < nEfi; i++) {
        if (efiRegions[i].type != 1 && efiRegions[i].type != 2) {
            continue;
        }
        uint64_t regionEnd = efiRegions[i].base + efiRegions[i].length;
        if (efiRegions[i].base <= base && end <= regionEnd) {
            return BOOT_OK;
        }
    }
    return BOOT_ERR_MEMMAP_OVERLAY;
}
