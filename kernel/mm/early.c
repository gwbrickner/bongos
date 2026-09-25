/* The early bump allocator (D-080, ROADMAP M2.2 step 1): the only memory source pmmInit() has
 * before the buddy allocator exists, used to back the Page array and its own page tables. Walks
 * `PmmMap.usable` top-down (highest range first), so it eats into NORMAL-zone memory before
 * DMA32 -- DMA32 is the scarcer, device-reserved resource. Every allocation is zeroed, so a Page
 * array page mapped straight from here needs no separate init pass (its zero state is already
 * PAGE_STATE_RESERVED, page.h). Internal to kernel/mm -- only pmm.c calls this. */
#include "panic.h"
#include "pmm-internal.h"

#include <stdint.h>

static PmmMap *earlyMap;
static uint64_t earlyHhdmBase;
static uint64_t
    earlyCursor[PMM_MAX_USABLE_RANGES]; /* per-range: pfn boundary of what's still free */
static bool earlySealed;
static uint64_t earlyUsedPages;

/* Boot-time only, not reentrant. Does not copy `map`; pmmInit() must keep it alive (and stop
 * mutating it itself) for the rest of the bump-allocator phase. */
void pmmEarlyInit(PmmMap *map, uint64_t hhdmBase) {
    earlyMap = map;
    earlyHhdmBase = hhdmBase;
    for (uint32_t i = 0; i < map->usableCount; i++) {
        earlyCursor[i] = map->usable[i].endPfn;
    }
    earlySealed = false;
    earlyUsedPages = 0;
}

/* Qword fill rather than a byte-at-a-time memset (kernel/core/string.c's memset is byte-wise, and
 * this runs under TCG during early boot on every span's worth of Page-array pages -- a qword loop
 * is a straightforward multiple-of-8-bytes win with no correctness cost, since every allocation
 * here is already 4 KiB-aligned). */
static void zeroPhysPages(uint64_t phys, uint64_t count) {
    uint64_t *p = (uint64_t *)(uintptr_t)(earlyHhdmBase + phys);
    uint64_t words = (count << 12) / sizeof(uint64_t);
    for (uint64_t i = 0; i < words; i++) {
        p[i] = 0;
    }
}

Status pmmEarlyAllocPages(uint64_t count, uint64_t *outPhys) {
    if (earlySealed) {
        panic("pmm: pmmEarlyAllocPages() called after pmmEarlySeal()");
    }
    for (uint32_t j = 0; j < earlyMap->usableCount; j++) {
        uint32_t i = earlyMap->usableCount - 1 - j; /* highest range first */
        uint64_t start = earlyMap->usable[i].startPfn;
        uint64_t avail = earlyCursor[i] - start;
        if (avail >= count) {
            earlyCursor[i] -= count;
            uint64_t phys = earlyCursor[i] << 12;
            zeroPhysPages(phys, count);
            earlyUsedPages += count;
            *outPhys = phys;
            return STATUS_OK;
        }
    }
    return STATUS_ERR_NO_MEMORY;
}

void pmmEarlySeal(void) {
    if (earlySealed) {
        return;
    }
    earlySealed = true;
    /* Shrink each usable range down to what's still actually free: pmmInit() frees
     * [usable[i].startPfn, usable[i].endPfn) to the buddy allocator next, and everything above
     * that (what earlyCursor consumed) must stay PAGE_STATE_RESERVED forever. */
    for (uint32_t i = 0; i < earlyMap->usableCount; i++) {
        earlyMap->usable[i].endPfn = earlyCursor[i];
    }
}

uint64_t pmmEarlyUsedPages(void) {
    return earlyUsedPages;
}
