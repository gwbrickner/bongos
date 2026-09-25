/* Internal types shared between kernel/mm's sources (pmm-map.c, early.c, buddy.c, pmm.c). Nothing
 * outside kernel/mm/ (and tests/host/, for the pure pieces) includes this -- kernel/include/pmm.h
 * is the public surface. */
#ifndef KERNEL_MM_PMM_INTERNAL_H
#define KERNEL_MM_PMM_INTERNAL_H

#include "bootinfo.h"
#include "list.h"
#include "page.h"
#include "pmm.h"
#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

/* pfn boundary between the DMA32 and NORMAL zones: 4 GiB / 4 KiB. A buddy block is naturally
 * aligned and at most 2^PMM_MAX_ORDER frames, so this must stay a multiple of that or a block
 * could straddle the boundary. */
#define PMM_ZONE_DMA32_END_PFN (0x100000000ULL >> 12)
_Static_assert(PMM_ZONE_DMA32_END_PFN % (1ull << PMM_MAX_ORDER) == 0,
               "DMA32/NORMAL boundary must be order-10 aligned");

static inline PmmZoneId pmmZoneOfPfn(uint64_t pfn) {
    return pfn < PMM_ZONE_DMA32_END_PFN ? PMM_ZONE_DMA32 : PMM_ZONE_NORMAL;
}

/* --- pmm-map.c: pure BootInfo-map scan (host-tested by tests/host/kernel_pmm_map_test.c) --- */

#define PMM_MAX_SPANS          128
#define PMM_MAX_USABLE_RANGES  256
#define PMM_MAX_RECLAIM_RANGES 16

typedef struct {
    uint64_t startPfn, endPfn; /* [start, end), order-10 aligned and merged */
} PmmSpan;

typedef struct {
    uint64_t startPfn, endPfn; /* USABLE, clipped to [256 (1 MiB), pmmLimitPfn) */
} PmmUsableRange;

typedef struct {
    uint64_t physBase, length; /* raw BOOT_MEM_LOADER_RECLAIM region, unclipped */
} PmmReclaimRange;

typedef struct {
    PmmSpan spans[PMM_MAX_SPANS];
    uint32_t spanCount;
    uint64_t maxPfn; /* end of the last span; also the array's pfn upper bound */

    PmmUsableRange usable[PMM_MAX_USABLE_RANGES];
    uint32_t usableCount;

    PmmReclaimRange loaderReclaim[PMM_MAX_RECLAIM_RANGES];
    uint32_t loaderReclaimCount;

    uint64_t lowReservedPages;                    /* USABLE below 1 MiB */
    uint64_t unmappedPages;                       /* USABLE at/beyond the HHDM window */
    uint64_t typePages[BOOT_MEM_FRAMEBUFFER + 1]; /* raw map totals, in pages, by BootMemType */
} PmmMap;

/* Scans `regions[0..count)` (sorted, non-overlapping, page-aligned -- the BootInfo contract) and
 * fills `*out` (zeroed first). Returns STATUS_ERR_INVALID if a region is misaligned, if `count`
 * would overflow PMM_MAX_USABLE_RANGES/PMM_MAX_RECLAIM_RANGES/PMM_MAX_SPANS, or if a region's type
 * is outside BootMemType's range -- with `*out` left as whatever was written so far (the caller
 * panics either way). Pure: no allocation, no I/O, no globals. No locks. */
Status pmmMapScan(const BootMemRegion *regions, uint32_t count, PmmMap *out);

/* True for the BootMemType values pmmMapScan() backs with Page entries (USABLE, LOADER_RECLAIM,
 * KERNEL, INITRD, ACPI_RECLAIM). Exposed so pmm.c's HHDM coverage check can walk the exact same
 * raw regions pmmMapScan() folded into spans -- never the *widened* spans themselves, whose
 * order-10 alignment padding can cover physical holes (e.g. the legacy 0xA0000-0x100000 VGA/BIOS
 * range) that never appeared in the BootInfo map at all and so carry no HHDM-mapping guarantee. */
bool pmmMapTypeIsManaged(uint32_t type);

/* --- early.c: the bump allocator (boot-time only, sealed before the buddy allocator opens) --- */

/* Initializes the bump allocator over `map->usable` (top-down per range, so NORMAL memory is
 * consumed before DMA32) and takes ownership of `map` for the rest of pmmInit(); zeroBase is the
 * HHDM base used to zero each allocation. Boot-time only, not reentrant. */
void pmmEarlyInit(PmmMap *map, uint64_t hhdmBase);

/* Allocates `count` contiguous, 4 KiB-aligned, zeroed physical pages from the highest-remaining
 * usable range with enough room, walking to progressively lower ranges as each is exhausted.
 * Returns STATUS_ERR_NO_MEMORY if no range (nor concatenation of a NEW lower range) has `count`
 * contiguous pages left -- callers only ever ask for 1 page at a time (the Page array is mapped
 * one 4 KiB leaf/table page at a time), so this never needs to search across a range boundary.
 * Boot-time only. */
Status pmmEarlyAllocPages(uint64_t count, uint64_t *outPhys);

/* Seals the bump allocator: every further pmmEarlyAllocPages() call panics. Also snapshots how
 * much of `map->usable` the bump allocator consumed, so pmmInit() can free the untouched
 * remainder via pmmAddFreeRange() and count `earlyPages`/`lowReservedPages`/`unmappedPages`
 * correctly. Boot-time only, idempotent. */
void pmmEarlySeal(void);

/* The number of pages the bump allocator has handed out so far (== earlyPages once sealed). */
uint64_t pmmEarlyUsedPages(void);

/* --- buddy.c: pure buddy-allocator core (host-tested by tests/host/kernel_buddy_test.c) --- */

typedef struct {
    ListNode freeList[PMM_ORDER_COUNT]; /* freeList[k]: heads of free 2^k-frame blocks */
    uint64_t freeBlocks[PMM_ORDER_COUNT];
    uint64_t freePages;    /* pages currently on this zone's free lists (excludes cached pages) */
    uint64_t managedPages; /* pages ever handed to this zone via buddyFreeBlock's initial free */
    uint64_t startPfn, endPfn;
    const char *name;
} PmmZone;

/* Initializes an empty zone (all free lists empty, no pages managed yet). No locks; pure. */
void buddyZoneInit(PmmZone *zone, const char *name, uint64_t startPfn, uint64_t endPfn);

/* Frees a 2^order-frame block starting at `pfn` (must be a multiple of 2^order, and every page of
 * the block -- head included -- must currently be PAGE_STATE_TAIL) into `zone`, merging with its
 * buddy at each order while the buddy is itself a free PAGE_STATE_BUDDY head of the same order.
 * Increments `zone->managedPages` and `zone->freePages` by 2^order. No locks; pure (touches only
 * the Page array and `zone`). Caller's responsibility: validate the block before calling (this
 * trusts its precondition and does not re-check it). */
void buddyFreeBlock(PmmZone *zone, uint64_t pfn, uint32_t order);

/* Takes the smallest available block of order >= `order` off `zone`'s free lists, splitting it
 * down to exactly `order` (pushing each split-off upper half back as its own free block). On
 * success, every page of the returned block -- head included -- is left as PAGE_STATE_TAIL (the
 * caller sets the head's real state) and `*outPfn` is its start; returns true. Returns false (zone
 * exhausted at every order >= `order`) with nothing changed otherwise. Decrements
 * `zone->freePages` by 2^order on success. No locks; pure. */
bool buddyAllocBlock(PmmZone *zone, uint32_t order, uint64_t *outPfn);

#endif
