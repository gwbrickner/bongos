/* See arch/early-map.h. Walks/extends whatever page tables CR3 currently points at -- the
 * loader's, at the point in boot pmmInit() runs (the kernel doesn't switch to its own PML4 until
 * M2.3) -- so every table page this allocates comes from the bump allocator (kernel/mm/early.c),
 * never the loader's own LOADER_RECLAIM pool: M2.3's reclaim of LOADER_RECLAIM would otherwise
 * free page tables the Page array still depends on. */
#include "arch/early-map.h"

#include "bootinfo.h"
#include "pte.h"

#include <arch/cpu.h>
#include <stdint.h>

static inline uint64_t *tableAt(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(BOOTINFO_HHDM_BASE + (phys & X86_PTE_ADDR_MASK));
}

/* Returns the next-level table for `table[idx]`, allocating and zeroing a fresh one via
 * `allocTable` if the entry is absent. A present entry with PS set is a conflict: something
 * already mapped a large leaf exactly where this walk needs a subtable -- never expected on the
 * Page-array VA range (kernel/mm/pmm.c never asks for anything else there), so treated as a bug in
 * the caller rather than something to route around. Non-leaf entries are always P|W only (SDM Vol
 * 3A §4.6: W is ANDed and XD is ORed down the walk, so only the leaf's own flags matter). */
static Status getOrAllocTable(uint64_t *table, uint32_t idx, ArchEarlyTableAllocFn allocTable,
                              uint64_t **outNext) {
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

Status archEarlyMapPage(uint64_t va, uint64_t pa, ArchEarlyTableAllocFn allocTable) {
    if (((va | pa) & (X86_PTE_SIZE_4K - 1)) != 0) {
        return STATUS_ERR_INVALID;
    }
    uint64_t *pml4 = tableAt(archReadCr3());

    uint64_t *pdpt, *pd, *pt;
    Status st = getOrAllocTable(pml4, (uint32_t)((va >> 39) & 511), allocTable, &pdpt);
    if (st != STATUS_OK) {
        return st;
    }
    st = getOrAllocTable(pdpt, (uint32_t)((va >> 30) & 511), allocTable, &pd);
    if (st != STATUS_OK) {
        return st;
    }
    st = getOrAllocTable(pd, (uint32_t)((va >> 21) & 511), allocTable, &pt);
    if (st != STATUS_OK) {
        return st;
    }

    uint32_t ptIdx = (uint32_t)((va >> 12) & 511);
    if (pt[ptIdx] & X86_PTE_P) {
        return STATUS_ERR_INVALID; /* only 0->1 transitions -- never overwrite an existing leaf */
    }
    pt[ptIdx] = (pa & X86_PTE_ADDR_MASK) | X86_PTE_FLAGS_PAGE_ARRAY;
    /* No invalidation needed or performed: SDM Vol 3A §4.10.4.3 -- no TLB or paging-structure-
     * cache entry is ever created from a not-present entry, so a fresh 0->1 mapping has nothing
     * stale to flush. */
    return STATUS_OK;
}

Status archEarlyLookup(uint64_t va, uint64_t *outPa, uint64_t *outLeafSize) {
    uint64_t *pml4 = tableAt(archReadCr3());
    uint64_t e = pml4[(va >> 39) & 511];
    if (!(e & X86_PTE_P)) {
        return STATUS_ERR_NOT_FOUND;
    }

    uint64_t *pdpt = tableAt(e);
    e = pdpt[(va >> 30) & 511];
    if (!(e & X86_PTE_P)) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (e & X86_PTE_PS) {
        /* SDM Vol 3A Table 4-15: for a 1 GiB leaf, bit 12 is PAT, not an address bit (bits 29:13
         * are reserved/must-be-0) -- masking with the plain 4 KiB X86_PTE_ADDR_MASK alone would
         * fold a set PAT bit into the computed physical address. */
        *outPa = (e & X86_PTE_ADDR_MASK & ~(X86_PTE_SIZE_1G - 1)) | (va & (X86_PTE_SIZE_1G - 1));
        *outLeafSize = X86_PTE_SIZE_1G;
        return STATUS_OK;
    }

    uint64_t *pd = tableAt(e);
    e = pd[(va >> 21) & 511];
    if (!(e & X86_PTE_P)) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (e & X86_PTE_PS) {
        /* Same PAT-bit reasoning as the 1 GiB case above (SDM Vol 3A Table 4-13). */
        *outPa = (e & X86_PTE_ADDR_MASK & ~(X86_PTE_SIZE_2M - 1)) | (va & (X86_PTE_SIZE_2M - 1));
        *outLeafSize = X86_PTE_SIZE_2M;
        return STATUS_OK;
    }

    uint64_t *pt = tableAt(e);
    e = pt[(va >> 12) & 511];
    if (!(e & X86_PTE_P)) {
        return STATUS_ERR_NOT_FOUND;
    }
    *outPa = (e & X86_PTE_ADDR_MASK) | (va & (X86_PTE_SIZE_4K - 1));
    *outLeafSize = X86_PTE_SIZE_4K;
    return STATUS_OK;
}
