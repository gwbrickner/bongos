/* See paging.h. */
#include "include/paging.h"

#include "include/bootmem.h"

BootStatus ptInit(PtBuilder *b, uint64_t poolPhys, uint32_t poolPages, bool has1G) {
    if (b == NULL || poolPages == 0) {
        return BOOT_ERR_NO_MEMORY;
    }
    bootMemset(bootPhysToPtr(poolPhys), 0, (size_t)poolPages * PT_SIZE_4K);
    b->poolPhys = poolPhys;
    b->poolPages = poolPages;
    b->poolUsed = 1; /* page 0 is the PML4 */
    b->has1G = has1G;
    b->pml4Phys = poolPhys;
    return BOOT_OK;
}

static BootStatus ptAllocPage(PtBuilder *b, uint64_t *outPhys) {
    if (b->poolUsed >= b->poolPages) {
        return BOOT_ERR_NO_MEMORY;
    }
    *outPhys = b->poolPhys + (uint64_t)b->poolUsed * PT_SIZE_4K;
    b->poolUsed++;
    return BOOT_OK;
}

/* Returns the next-level table for `table[idx]`, allocating a fresh (already-zeroed, from the
 * pool) one if the entry is absent. A present entry with PS set is a conflict: something already
 * mapped a large leaf exactly where this walk needs a subtable. Non-leaf entries are always
 * P|W only (SDM §4.6: W is ANDed and XD is ORed down the walk, so the leaf alone decides
 * writability/executability; NX/US/G on a non-leaf entry would only add confusion, never effect).
 */
static BootStatus ptGetOrAllocTable(PtBuilder *b, uint64_t *table, uint32_t idx,
                                    uint64_t **outNext) {
    uint64_t entry = table[idx];
    if (entry & PT_P) {
        if (entry & PT_PS) {
            return BOOT_ERR_PT_CONFLICT;
        }
        *outNext = (uint64_t *)bootPhysToPtr(entry & PT_ADDR_MASK);
        return BOOT_OK;
    }
    uint64_t newPhys;
    BootStatus st = ptAllocPage(b, &newPhys);
    if (st != BOOT_OK) {
        return st;
    }
    table[idx] = newPhys | PT_P | PT_W;
    *outNext = (uint64_t *)bootPhysToPtr(newPhys);
    return BOOT_OK;
}

/* Writes a leaf entry, refusing to overwrite (or overlay a large page atop) whatever is already
 * there -- this builder never merges or coalesces existing mappings. */
static BootStatus ptWriteLeaf(uint64_t *table, uint32_t idx, uint64_t value) {
    if (table[idx] & PT_P) {
        return BOOT_ERR_PT_CONFLICT;
    }
    table[idx] = value;
    return BOOT_OK;
}

BootStatus ptMapRange(PtBuilder *b, uint64_t va, uint64_t pa, uint64_t size, uint64_t leafFlags,
                      bool allowLarge) {
    /* A misaligned size would underflow the loop counter below (size -= PT_SIZE_4K past zero)
     * and keep mapping until the pool is exhausted instead of stopping cleanly. */
    if (((va | pa | size) & (PT_SIZE_4K - 1)) != 0) {
        return BOOT_ERR_PT_UNALIGNED;
    }

    uint64_t *pml4 = (uint64_t *)bootPhysToPtr(b->pml4Phys);

    while (size > 0) {
        uint32_t pml4Idx = (uint32_t)((va >> 39) & 511);
        uint64_t *pdpt;
        BootStatus st = ptGetOrAllocTable(b, pml4, pml4Idx, &pdpt);
        if (st != BOOT_OK) {
            return st;
        }

        if (allowLarge && b->has1G && (va & (PT_SIZE_1G - 1)) == 0 &&
            (pa & (PT_SIZE_1G - 1)) == 0 && size >= PT_SIZE_1G) {
            uint32_t pdptIdx = (uint32_t)((va >> 30) & 511);
            st = ptWriteLeaf(pdpt, pdptIdx, pa | leafFlags | PT_PS);
            if (st != BOOT_OK) {
                return st;
            }
            va += PT_SIZE_1G;
            pa += PT_SIZE_1G;
            size -= PT_SIZE_1G;
            continue;
        }

        uint32_t pdptIdx = (uint32_t)((va >> 30) & 511);
        uint64_t *pd;
        st = ptGetOrAllocTable(b, pdpt, pdptIdx, &pd);
        if (st != BOOT_OK) {
            return st;
        }

        if (allowLarge && (va & (PT_SIZE_2M - 1)) == 0 && (pa & (PT_SIZE_2M - 1)) == 0 &&
            size >= PT_SIZE_2M) {
            uint32_t pdIdx = (uint32_t)((va >> 21) & 511);
            st = ptWriteLeaf(pd, pdIdx, pa | leafFlags | PT_PS);
            if (st != BOOT_OK) {
                return st;
            }
            va += PT_SIZE_2M;
            pa += PT_SIZE_2M;
            size -= PT_SIZE_2M;
            continue;
        }

        uint32_t pdIdx = (uint32_t)((va >> 21) & 511);
        uint64_t *pt;
        st = ptGetOrAllocTable(b, pd, pdIdx, &pt);
        if (st != BOOT_OK) {
            return st;
        }

        uint32_t ptIdx = (uint32_t)((va >> 12) & 511);
        st = ptWriteLeaf(pt, ptIdx, pa | leafFlags);
        if (st != BOOT_OK) {
            return st;
        }
        va += PT_SIZE_4K;
        pa += PT_SIZE_4K;
        size -= PT_SIZE_4K;
    }
    return BOOT_OK;
}

BootStatus ptMapElfImage(PtBuilder *b, const ElfImage *img, uint64_t physBase, uint64_t slide) {
    for (uint32_t i = 0; i < img->segCount; i++) {
        const ElfSegment *s = &img->segs[i];
        uint64_t va = s->vaddr + slide;
        uint64_t pa = physBase + (s->vaddr - img->linkBase);
        uint64_t size = bootAlignUp(s->memsz, PT_SIZE_4K);
        uint64_t flags;
        if (s->flags & ELF_PF_X) {
            flags = PT_FLAGS_KERNEL_RX;
        } else if (s->flags & ELF_PF_W) {
            flags = PT_FLAGS_KERNEL_RW;
        } else {
            flags = PT_FLAGS_KERNEL_RO;
        }
        BootStatus st = ptMapRange(b, va, pa, size, flags, false);
        if (st != BOOT_OK) {
            return st;
        }
    }
    return BOOT_OK;
}

BootStatus ptLookup(const PtBuilder *b, uint64_t va, uint64_t *pa, uint64_t *leafFlags) {
    uint64_t *pml4 = (uint64_t *)bootPhysToPtr(b->pml4Phys);
    uint32_t pml4Idx = (uint32_t)((va >> 39) & 511);
    uint64_t e4 = pml4[pml4Idx];
    if (!(e4 & PT_P)) {
        return BOOT_ERR_NOT_MAPPED;
    }

    uint64_t *pdpt = (uint64_t *)bootPhysToPtr(e4 & PT_ADDR_MASK);
    uint32_t pdptIdx = (uint32_t)((va >> 30) & 511);
    uint64_t e3 = pdpt[pdptIdx];
    if (!(e3 & PT_P)) {
        return BOOT_ERR_NOT_MAPPED;
    }
    if (e3 & PT_PS) {
        *pa = (e3 & PT_ADDR_MASK) + (va & (PT_SIZE_1G - 1));
        *leafFlags = e3 & ~PT_ADDR_MASK;
        return BOOT_OK;
    }

    uint64_t *pd = (uint64_t *)bootPhysToPtr(e3 & PT_ADDR_MASK);
    uint32_t pdIdx = (uint32_t)((va >> 21) & 511);
    uint64_t e2 = pd[pdIdx];
    if (!(e2 & PT_P)) {
        return BOOT_ERR_NOT_MAPPED;
    }
    if (e2 & PT_PS) {
        *pa = (e2 & PT_ADDR_MASK) + (va & (PT_SIZE_2M - 1));
        *leafFlags = e2 & ~PT_ADDR_MASK;
        return BOOT_OK;
    }

    uint64_t *pt = (uint64_t *)bootPhysToPtr(e2 & PT_ADDR_MASK);
    uint32_t ptIdx = (uint32_t)((va >> 12) & 511);
    uint64_t e1 = pt[ptIdx];
    if (!(e1 & PT_P)) {
        return BOOT_ERR_NOT_MAPPED;
    }
    *pa = (e1 & PT_ADDR_MASK) + (va & (PT_SIZE_4K - 1));
    *leafFlags = e1 & ~PT_ADDR_MASK;
    return BOOT_OK;
}
