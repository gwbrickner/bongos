/* The physical memory manager (D-079..D-082, ROADMAP M2.2): pmmInit() ties the pure pmm-map.c
 * scan, the early.c bump allocator, and the buddy.c core together, then this file exposes pmm.h's
 * public alloc/free/stats API on top of a BSP-only per-CPU cache. */
#include "arch/early-map.h"
#include "bootinfo-validate.h" /* bootMemTypeName */
#include "klog.h"
#include "panic.h"
#include "pmm-internal.h"
#include "vmm.h" /* vmmKernelTablesActive() -- pmmReclaimLoaderMemory()'s precondition, D-089 */

#include <arch/cpu.h>
#include <stdint.h>

static PmmMap pmmMap;
static uint64_t pmmHhdmBaseValue;
static PmmZone pmmZones[PMM_ZONE_COUNT];
static uint64_t pmmPageArrayPages;
static uint64_t pmmPageTablePages;
static uint64_t pmmReclaimedPagesValue;
static PmmBugKind pmmLastBugKind = PMM_BUG_NONE;

/* --- BSP-only per-CPU cache: order-0 only, one free list per zone (D-081). No cpu index appears
 * in any call -- pmmLocalCache() is the one place that knows there's only the BSP today; M3.5
 * replaces its body with a real `cpuLocal()->pmmCache[zone]` lookup and nothing else in this file
 * changes. */
typedef struct {
    ListNode pages;
    uint32_t count;
} PmmPcpList;

#define PMM_PCP_BATCH 32
#define PMM_PCP_HIGH  (6 * PMM_PCP_BATCH)

static PmmPcpList pmmBspCache[PMM_ZONE_COUNT];

static PmmPcpList *pmmLocalCache(PmmZoneId zone) {
    return &pmmBspCache[zone];
}

static void pmmCacheInitAll(void) {
    for (uint32_t z = 0; z < PMM_ZONE_COUNT; z++) {
        listInit(&pmmBspCache[z].pages);
        pmmBspCache[z].count = 0;
    }
}

/* --- lock: IRQ-disable only (M2.2: single CPU, IF stays 0 anyway per ARCHITECTURE §7.1 until
 * M3.2) -- a real spinlock arrives with SMP (M3.4/M3.5). Every entry point below still follows the
 * IRQ-safe contract now so that later upgrade needs no rewrite. */
static uint64_t pmmLock(void) {
    return archIrqSave();
}
static void pmmUnlock(uint64_t flags) {
    archIrqRestore(flags);
}

#ifdef KERNEL_DEBUG
#define PMM_POISON 0x6B6B6B6B6B6B6B6BULL

static void pmmPoisonPage(Page *page) {
    uint64_t *p = (uint64_t *)(uintptr_t)(pmmHhdmBaseValue + pmmPageToPhys(page));
    for (uint64_t i = 0; i < 4096 / sizeof(uint64_t); i++) {
        p[i] = PMM_POISON;
    }
    page->flags |= PAGE_F_POISONED;
}
#endif

static _Noreturn void pmmBug(PmmBugKind kind, uint64_t pfn) {
    pmmLastBugKind = kind;
    static const char *const names[] = {
        [PMM_BUG_NONE] = "none",
        [PMM_BUG_INVALID_PAGE] = "invalid page",
        [PMM_BUG_BAD_ORDER] = "bad order",
        [PMM_BUG_MISALIGNED] = "misaligned",
        [PMM_BUG_ORDER_MISMATCH] = "order mismatch",
        [PMM_BUG_DOUBLE_FREE] = "double free",
        [PMM_BUG_NOT_HEAD] = "not a block head",
        [PMM_BUG_RESERVED_FRAME] = "reserved frame",
        [PMM_BUG_CORRUPT_STATE] = "corrupt state",
        [PMM_BUG_POISON] = "poison mismatch (write after free)",
        [PMM_BUG_OWNED_PAGE] = "owned page (slab/vmalloc never released it)",
    };
    panicBug("pmm: %s pfn=0x%llx", names[kind], (unsigned long long)pfn);
}

#ifdef KERNEL_DEBUG
/* Only called from pmmAllocPages, after the lock is released (pmmBug must never fire with
 * pmmLock held -- archTrapCatch's resume is a longjmp that would skip pmmUnlock() entirely and
 * leave interrupts disabled forever). */
static void pmmCheckAndClearPoison(Page *page) {
    if (!(page->flags & PAGE_F_POISONED)) {
        return;
    }
    uint64_t *p = (uint64_t *)(uintptr_t)(pmmHhdmBaseValue + pmmPageToPhys(page));
    /* Every qword, not just the first/last: a write-after-free that only touches an interior
     * offset (the common case -- a struct field, not the whole page) must still be caught. Same
     * per-page cost as pmmPoisonPage()'s own fill loop. */
    for (uint64_t i = 0; i < 4096 / sizeof(uint64_t); i++) {
        if (p[i] != PMM_POISON) {
            pmmBug(PMM_BUG_POISON, pageToPfn(page));
        }
    }
    page->flags = (uint16_t)(page->flags & ~PAGE_F_POISONED);
}
#endif

bool pmmPfnValid(uint64_t pfn) {
    uint32_t lo = 0, hi = pmmMap.spanCount;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const PmmSpan *s = &pmmMap.spans[mid];
        if (pfn < s->startPfn) {
            hi = mid;
        } else if (pfn >= s->endPfn) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

Page *pmmPhysToPage(uint64_t phys) {
    uint64_t pfn = phys >> 12;
    return pmmPfnValid(pfn) ? pageFromPfn(pfn) : NULL;
}

bool pmmPhysIsEarlyAlloc(uint64_t phys) {
    return pmmEarlyWasAllocated(phys >> 12);
}

uint64_t pmmHhdmBase(void) {
    return pmmHhdmBaseValue;
}

/* Validates a pmmFreePages() call against the Page-state machine and returns the block's pfn via
 * `*outPfn` on success. Deliberately called with pmmLock NOT held (see pmmBug's own comment) --
 * every failure path below calls pmmBug(), which never returns. */
static void pmmValidateForFree(Page *page, uint32_t order, uint64_t *outPfn) {
    if (page == NULL) {
        pmmBug(PMM_BUG_INVALID_PAGE, 0);
    }
    uintptr_t addr = (uintptr_t)page;
    if (addr < PAGE_ARRAY_VA || (addr - PAGE_ARRAY_VA) % sizeof(Page) != 0) {
        pmmBug(PMM_BUG_INVALID_PAGE, 0);
    }
    uint64_t pfn = pageToPfn(page);
    if (!pmmPfnValid(pfn)) {
        pmmBug(PMM_BUG_INVALID_PAGE, pfn);
    }
    if (order > PMM_MAX_ORDER) {
        pmmBug(PMM_BUG_BAD_ORDER, pfn);
    }
    if ((pfn & (((uint64_t)1 << order) - 1)) != 0) {
        pmmBug(PMM_BUG_MISALIGNED, pfn);
    }

    switch (page->state) {
        case PAGE_STATE_ALLOCATED:
            if (page->order != order) {
                pmmBug(PMM_BUG_ORDER_MISMATCH, pfn);
            }
            /* M2.4, D-095: an owner (the slab allocator or vmalloc) must clear its Page.flags
             * ownership bit on every page of the block before ever calling pmmFreePages() -- a
             * page still carrying one here means that owner never released it (or the caller is
             * freeing someone else's live memory directly), either way a kernel bug. */
            for (uint64_t p = pfn; p < pfn + ((uint64_t)1 << order); p++) {
                if (pageFromPfn(p)->flags & PAGE_F_OWNER_MASK) {
                    pmmBug(PMM_BUG_OWNED_PAGE, p);
                }
            }
            *outPfn = pfn;
            return;
        case PAGE_STATE_BUDDY:
        case PAGE_STATE_PCP:
            pmmBug(PMM_BUG_DOUBLE_FREE, pfn);
        case PAGE_STATE_TAIL: {
            /* Is `pfn` covered by some block that's *already* free? For every alignment k, the
             * k-aligned floor of `pfn` is a multiple of 2^k; if that candidate head is a free BUDDY
             * block of order >= k, then (both being multiples of 2^k) pfn must lie inside
             * [candidate, candidate + 2^order) -- i.e. this exact frame was already returned to the
             * allocator, so freeing it again is a double free. If no k finds such a block, `pfn` is
             * simply an interior page of a still-live ALLOCATED block -- a caller bug, but not
             * (yet) a double free. */
            for (uint32_t k = 0; k <= PMM_MAX_ORDER; k++) {
                uint64_t headPfn = pfn & ~(((uint64_t)1 << k) - 1);
                Page *head = pageFromPfn(headPfn);
                if (head->state == PAGE_STATE_BUDDY && head->order >= k) {
                    pmmBug(PMM_BUG_DOUBLE_FREE, pfn);
                }
            }
            pmmBug(PMM_BUG_NOT_HEAD, pfn);
        }
        case PAGE_STATE_RESERVED:
            pmmBug(PMM_BUG_RESERVED_FRAME, pfn);
        default:
            pmmBug(PMM_BUG_CORRUPT_STATE, pfn);
    }
}

static void pmmZeroBlock(uint64_t pfn, uint32_t order) {
    uint64_t *p = (uint64_t *)(uintptr_t)(pmmHhdmBaseValue + (pfn << 12));
    uint64_t words = (((uint64_t)1 << order) << 12) / sizeof(uint64_t);
    for (uint64_t i = 0; i < words; i++) {
        p[i] = 0;
    }
}

static void pmmZoneListFor(PmmFlags flags, PmmZoneId zonesOut[2], uint32_t *count) {
    if (flags & PMM_FLAG_DMA32) {
        zonesOut[0] = PMM_ZONE_DMA32;
        *count = 1;
    } else {
        zonesOut[0] = PMM_ZONE_NORMAL;
        zonesOut[1] = PMM_ZONE_DMA32;
        *count = 2;
    }
}

static bool pmmCacheAllocLocked(PmmZoneId zone, uint64_t *outPfn) {
    PmmPcpList *cache = pmmLocalCache(zone);
    if (cache->count == 0) {
        for (uint32_t i = 0; i < PMM_PCP_BATCH; i++) {
            uint64_t pfn;
            if (!buddyAllocBlock(&pmmZones[zone], 0, &pfn)) {
                break;
            }
            Page *p = pageFromPfn(pfn);
            p->state = PAGE_STATE_PCP;
            listPushHead(&cache->pages, &p->lru);
            cache->count++;
        }
        if (cache->count == 0) {
            return false;
        }
    }
    ListNode *node = listPopHead(&cache->pages);
    cache->count--;
    *outPfn = pageToPfn(LIST_CONTAINER(node, Page, lru));
    return true;
}

static void pmmCacheFreeLocked(PmmZoneId zone, uint64_t pfn) {
    PmmPcpList *cache = pmmLocalCache(zone);
    Page *p = pageFromPfn(pfn);
    p->state = PAGE_STATE_PCP;
    p->order = 0;
    p->refcount = 0;
    listPushHead(&cache->pages, &p->lru);
    cache->count++;
    if (cache->count > PMM_PCP_HIGH) {
        for (uint32_t i = 0; i < PMM_PCP_BATCH; i++) {
            ListNode *node = listPopTail(&cache->pages);
            if (node == NULL) {
                break;
            }
            cache->count--;
            Page *tail = LIST_CONTAINER(node, Page, lru);
            tail->state = PAGE_STATE_TAIL;
            buddyFreeBlock(&pmmZones[zone], pageToPfn(tail), 0);
        }
    }
}

void pmmDrainLocalCache(void) {
    uint64_t flags = pmmLock();
    for (uint32_t z = 0; z < PMM_ZONE_COUNT; z++) {
        PmmPcpList *cache = pmmLocalCache(z);
        ListNode *node;
        while ((node = listPopHead(&cache->pages)) != NULL) {
            cache->count--;
            Page *p = LIST_CONTAINER(node, Page, lru);
            p->state = PAGE_STATE_TAIL;
            buddyFreeBlock(&pmmZones[z], pageToPfn(p), 0);
        }
    }
    pmmUnlock(flags);
}

Status pmmAllocPages(uint32_t order, PmmFlags flags, Page **outPage) {
    if (outPage == NULL) {
        return STATUS_ERR_INVALID;
    }
    *outPage = NULL;
    if (order > PMM_MAX_ORDER || (flags & ~(PmmFlags)PMM_FLAGS_VALID) != 0) {
        return STATUS_ERR_INVALID;
    }

    PmmZoneId zoneList[2];
    uint32_t zoneCount;
    pmmZoneListFor(flags, zoneList, &zoneCount);

    uint64_t irqFlags = pmmLock();
    uint64_t pfn = 0;
    bool ok = false;
    for (uint32_t i = 0; i < zoneCount && !ok; i++) {
        ok = (order == 0) ? pmmCacheAllocLocked(zoneList[i], &pfn)
                          : buddyAllocBlock(&pmmZones[zoneList[i]], order, &pfn);
    }
    if (!ok) {
        pmmUnlock(irqFlags);
        return STATUS_ERR_NO_MEMORY;
    }

    Page *head = pageFromPfn(pfn);
    head->state = PAGE_STATE_ALLOCATED;
    head->order = (uint8_t)order;
    head->refcount = 1;
    head->mapcount = 0;
    head->object = NULL;
    head->objectIndex = 0;
    head->privateWord = 0;
    pmmUnlock(irqFlags);

#ifdef KERNEL_DEBUG
    for (uint64_t p = pfn; p < pfn + ((uint64_t)1 << order); p++) {
        pmmCheckAndClearPoison(pageFromPfn(p));
    }
#endif
    if (flags & PMM_FLAG_ZERO) {
        pmmZeroBlock(pfn, order);
    }

    *outPage = head;
    return STATUS_OK;
}

void pmmFreePages(Page *page, uint32_t order) {
    uint64_t pfn;
    pmmValidateForFree(page, order, &pfn); /* unlocked; never returns on failure */

    uint64_t irqFlags = pmmLock();
#ifdef KERNEL_DEBUG
    for (uint64_t p = pfn; p < pfn + ((uint64_t)1 << order); p++) {
        pmmPoisonPage(pageFromPfn(p));
    }
#endif
    if (order == 0) {
        pmmCacheFreeLocked(pmmZoneOfPfn(pfn), pfn);
    } else {
        /* buddyFreeBlock()'s precondition is that every page of the block -- head included -- is
         * already PAGE_STATE_TAIL (pmm-internal.h). The head is the one page that isn't: it's
         * still PAGE_STATE_ALLOCATED from pmmAllocPages(). If this block merges with its buddy and
         * the merge moves the block's base *down* to the buddy's pfn (i.e. this block was the
         * upper half), buddyFreeBlock only ever writes the *final* merged head's Page -- it never
         * touches this original pfn again, leaving it stuck at ALLOCATED with a stale order. A
         * second pmmFreePages() on that stale page would then pass validation (state ALLOCATED,
         * order still matches) and corrupt the free lists. Reset it here, unconditionally, before
         * the merge walk ever runs. */
        Page *head = pageFromPfn(pfn);
        head->state = PAGE_STATE_TAIL;
        head->order = 0;
        head->refcount = 0;
        buddyFreeBlock(&pmmZones[pmmZoneOfPfn(pfn)], pfn, order);
    }
    pmmUnlock(irqFlags);
}

Status pmmAddFreeRange(uint64_t physBase, uint64_t length) {
    if (((physBase | length) & 0xFFF) != 0) {
        return STATUS_ERR_INVALID;
    }
    if (length == 0) {
        return STATUS_OK;
    }
    uint64_t startPfn = physBase >> 12;
    uint64_t endPfn = startPfn + (length >> 12);
    if (endPfn <= startPfn) {
        /* Defensive only: `physBase`/`length` are already validated 4 KiB-aligned uint64_t
         * values, so this can't actually overflow with any address this platform can produce --
         * but reject explicitly rather than silently doing nothing if that ever changes, instead
         * of relying on the per-frame pmmPfnValid() loop below to happen to catch it too. */
        return STATUS_ERR_INVALID;
    }

    for (uint64_t p = startPfn; p < endPfn; p++) {
        if (!pmmPfnValid(p) || pageFromPfn(p)->state != PAGE_STATE_RESERVED) {
            return STATUS_ERR_INVALID;
        }
    }

    uint64_t irqFlags = pmmLock();
    uint64_t pfn = startPfn;
    while (pfn < endPfn) {
        uint32_t order = (pfn == 0) ? PMM_MAX_ORDER : (uint32_t)__builtin_ctzll(pfn);
        if (order > PMM_MAX_ORDER) {
            order = PMM_MAX_ORDER;
        }
        while (((uint64_t)1 << order) > endPfn - pfn) {
            order--;
        }
        for (uint64_t p = pfn; p < pfn + ((uint64_t)1 << order); p++) {
            pageFromPfn(p)->state = PAGE_STATE_TAIL;
        }
        PmmZone *zone = &pmmZones[pmmZoneOfPfn(pfn)];
        zone->managedPages += (uint64_t)1 << order; /* this range is entering the zone for the
                                                     * first time -- ordinary frees never do
                                                     * this (buddyFreeBlock's own contract) */
        buddyFreeBlock(zone, pfn, order);
        pfn += (uint64_t)1 << order;
    }
    pmmUnlock(irqFlags);
    return STATUS_OK;
}

/* PMM_RECLAIM_LOW_LIMIT mirrors D-080's low-memory withholding (PMM_LOW_MEM_LIMIT_PFN, pmm-map.c):
 * pmmReclaimLoaderMemory() must never hand anything below 1 MiB to the buddy allocator either. */
#define PMM_RECLAIM_LOW_LIMIT 0x100000ULL

static void pmmZeroRange(uint64_t physBase, uint64_t length) {
    uint64_t *p = (uint64_t *)(uintptr_t)(pmmHhdmBaseValue + physBase);
    uint64_t words = length / sizeof(uint64_t);
    for (uint64_t i = 0; i < words; i++) {
        p[i] = 0;
    }
}

/* True if every frame in [base, base+length) is still PAGE_STATE_RESERVED -- checked *before*
 * pmmReclaimPiece() zeroes anything, so a mismatch between pmmMap.loaderReclaim[] and the Page
 * array's actual state (which should never happen, but would mean this range isn't safe to
 * reclaim at all) is caught before any byte is destroyed, not after. `pmmAddFreeRange()` re-checks
 * this same condition itself right before it commits, but only after this function has already
 * zeroed the range -- validating here first is what keeps a caught inconsistency's evidence (and
 * whatever live data a wrongly-included frame held) intact for the panic report. */
static bool pmmRangeAllReserved(uint64_t base, uint64_t length) {
    uint64_t startPfn = base >> 12;
    uint64_t endPfn = startPfn + (length >> 12);
    for (uint64_t pfn = startPfn; pfn < endPfn; pfn++) {
        if (!pmmPfnValid(pfn) || pageFromPfn(pfn)->state != PAGE_STATE_RESERVED) {
            return false;
        }
    }
    return true;
}

/* Frees [base, base+length) to the buddy allocator, splitting around [keepPagePhys,
 * keepPagePhys+4096) if it falls inside (recurses at most once per side -- the two split pieces
 * can never contain keepPagePhys again). Zeroes each freed piece through the HHDM first: a loader
 * stack or its page-table pool can hold RNG/seed residue or other loader-controlled data. Returns
 * the number of pages actually freed. Panics (via pmmAddFreeRange's contract, and its own
 * pmmRangeAllReserved() precheck) rather than return an error -- pmmMap.loaderReclaim[] ranges are
 * the pmm's own record of exactly what pmmInit() scanned, so a failure here means that record is
 * inconsistent with the Page array, not a bad caller argument. */
static uint64_t pmmReclaimPiece(uint64_t base, uint64_t length, uint64_t keepPagePhys) {
    if (length == 0) {
        return 0;
    }
    uint64_t end = base + length;
    uint64_t keepEnd = keepPagePhys + 4096;
    if (keepPagePhys >= base && keepPagePhys < end) {
        uint64_t total = 0;
        if (keepPagePhys > base) {
            total += pmmReclaimPiece(base, keepPagePhys - base, keepPagePhys);
        }
        if (keepEnd < end) {
            total += pmmReclaimPiece(keepEnd, end - keepEnd, keepPagePhys);
        }
        return total;
    }

    if (!pmmRangeAllReserved(base, length)) {
        panic("pmm: reclaim: [0x%llx, 0x%llx) is not entirely PAGE_STATE_RESERVED",
              (unsigned long long)base, (unsigned long long)end);
    }
    pmmZeroRange(base, length);
    Status st = pmmAddFreeRange(base, length);
    if (st != STATUS_OK) {
        panic("pmm: reclaim: pmmAddFreeRange(0x%llx, 0x%llx) failed (status %d)",
              (unsigned long long)base, (unsigned long long)length, (int)st);
    }
    return length >> 12;
}

void pmmReclaimLoaderMemory(uint64_t keepPagePhys) {
    static bool reclaimDone = false;
    if (reclaimDone) {
        panic("pmm: pmmReclaimLoaderMemory: called twice");
    }
    if (!vmmKernelTablesActive()) {
        panic("pmm: pmmReclaimLoaderMemory: called before the kernel's own page tables are active");
    }
    if ((keepPagePhys & 0xFFF) != 0) {
        panic("pmm: pmmReclaimLoaderMemory: keepPagePhys is not 4 KiB aligned");
    }
    reclaimDone = true;

    uint64_t reclaimed = 0;
    for (uint32_t i = 0; i < pmmMap.loaderReclaimCount; i++) {
        uint64_t base = pmmMap.loaderReclaim[i].physBase;
        uint64_t end = base + pmmMap.loaderReclaim[i].length;
        if (base < PMM_RECLAIM_LOW_LIMIT) {
            base = PMM_RECLAIM_LOW_LIMIT;
        }
        if (end > BOOTINFO_HHDM_SIZE) { /* mirrors pmm-map.c's own HHDM-window clip */
            end = BOOTINFO_HHDM_SIZE;
        }
        if (base >= end) {
            continue;
        }
        reclaimed += pmmReclaimPiece(base, end - base, keepPagePhys);
    }

    /* Independent sanity bound (not a full re-derivation, which would just repeat the loop
     * above): reclaiming can never exceed the raw LOADER_RECLAIM total pmmMapScan() recorded at
     * boot, before any of this function's own clipping/splitting logic ran. Catches, for example,
     * a bug that double-counts a piece or free a range into the wrong zone's ledger. Checked
     * *before* publishing `reclaimed` into pmmReclaimedPagesValue, so a panic here never leaves
     * the stats snapshot other code reads (pmmGetStats()) reflecting a count this very check just
     * proved untrustworthy. */
    if (reclaimed > pmmMap.typePages[BOOT_MEM_LOADER_RECLAIM]) {
        panic("pmm: reclaim: reclaimed %llu pages, more than the %llu ever recorded as "
              "LOADER_RECLAIM",
              (unsigned long long)reclaimed,
              (unsigned long long)pmmMap.typePages[BOOT_MEM_LOADER_RECLAIM]);
    }
    pmmReclaimedPagesValue += reclaimed;

    klogWrite(KLOG_INFO, "pmm", "reclaimed %llu KiB of LOADER_RECLAIM (kept BootInfo page 0x%llx)",
              (unsigned long long)reclaimed * 4, (unsigned long long)keepPagePhys);
}

void pmmGetStats(PmmStats *out) {
    uint64_t irqFlags = pmmLock();
    *out = (PmmStats){0};
    for (uint32_t t = 0; t <= BOOT_MEM_FRAMEBUFFER; t++) {
        out->typePages[t] = pmmMap.typePages[t];
    }
    out->usablePages = pmmMap.typePages[BOOT_MEM_USABLE];
    out->lowReservedPages = pmmMap.lowReservedPages;
    out->unmappedPages = pmmMap.unmappedPages;
    out->reclaimedPages = pmmReclaimedPagesValue;
    out->earlyPages = pmmEarlyUsedPages();
    out->pageArrayPages = pmmPageArrayPages;
    out->pageTablePages = pmmPageTablePages;

    for (uint32_t z = 0; z < PMM_ZONE_COUNT; z++) {
        uint64_t zoneFree = pmmZones[z].freePages + pmmLocalCache(z)->count;
        out->zoneManagedPages[z] = pmmZones[z].managedPages;
        out->zoneFreePages[z] = zoneFree;
        out->managedPages += pmmZones[z].managedPages;
        out->freePages += zoneFree;
        out->cachedPages += pmmLocalCache(z)->count;
    }
    out->allocatedPages = out->managedPages - out->freePages;
    pmmUnlock(irqFlags);
}

void pmmPrintMeminfo(void) {
    PmmStats s;
    pmmGetStats(&s);

    klogWrite(KLOG_INFO, "meminfo", "MemTotal:    %llu kB", (unsigned long long)s.usablePages * 4);
    klogWrite(KLOG_INFO, "meminfo", "MemFree:     %llu kB (cached: %llu kB)",
              (unsigned long long)s.freePages * 4, (unsigned long long)s.cachedPages * 4);
    klogWrite(KLOG_INFO, "meminfo", "MemManaged:  %llu kB", (unsigned long long)s.managedPages * 4);
    klogWrite(KLOG_INFO, "meminfo", "PageArray:   %llu kB (array %llu kB + tables %llu kB)",
              (unsigned long long)s.earlyPages * 4, (unsigned long long)s.pageArrayPages * 4,
              (unsigned long long)s.pageTablePages * 4);
    klogWrite(KLOG_INFO, "meminfo", "LowReserved: %llu kB",
              (unsigned long long)s.lowReservedPages * 4);
    klogWrite(KLOG_INFO, "meminfo", "Unmapped:    %llu kB",
              (unsigned long long)s.unmappedPages * 4);
    klogWrite(KLOG_INFO, "meminfo", "Reclaimed:   %llu kB",
              (unsigned long long)s.reclaimedPages * 4);
    for (uint32_t t = BOOT_MEM_USABLE; t <= BOOT_MEM_FRAMEBUFFER; t++) {
        if (t == BOOT_MEM_USABLE || s.typePages[t] == 0) {
            continue;
        }
        klogWrite(KLOG_INFO, "meminfo", "%s: %llu kB", bootMemTypeName(t),
                  (unsigned long long)s.typePages[t] * 4);
    }
    klogWrite(KLOG_INFO, "meminfo", "Zone DMA32:  free %llu kB of %llu kB",
              (unsigned long long)s.zoneFreePages[PMM_ZONE_DMA32] * 4,
              (unsigned long long)s.zoneManagedPages[PMM_ZONE_DMA32] * 4);
    klogWrite(KLOG_INFO, "meminfo", "Zone NORMAL: free %llu kB of %llu kB",
              (unsigned long long)s.zoneFreePages[PMM_ZONE_NORMAL] * 4,
              (unsigned long long)s.zoneManagedPages[PMM_ZONE_NORMAL] * 4);

    /* M2.3, D-089: reclaiming LOADER_RECLAIM moves pages into MemManaged without ever having been
     * part of MemTotal (a disjoint BootInfo type from USABLE), so the identity gains a term on the
     * left rather than losing one on the right -- strictly stronger, not weaker (CLAUDE.md: never
     * weaken a test to get a pass). Before any reclaim has run, reclaimedPages is 0 and this is
     * exactly the M2.2 formula. */
    uint64_t sum = s.managedPages + s.earlyPages + s.lowReservedPages + s.unmappedPages;
    bool ok = sum == s.usablePages + s.reclaimedPages;
    klogWrite(ok ? KLOG_INFO : KLOG_ERROR, "meminfo",
              "check: MemTotal + Reclaimed == MemManaged + PageArray + LowReserved + Unmapped: %s",
              ok ? "OK" : "MISMATCH");
    if (!ok) {
        panic("pmm: meminfo self-check failed (usable=%llu reclaimed=%llu managed=%llu early=%llu "
              "low=%llu unmapped=%llu)",
              (unsigned long long)s.usablePages, (unsigned long long)s.reclaimedPages,
              (unsigned long long)s.managedPages, (unsigned long long)s.earlyPages,
              (unsigned long long)s.lowReservedPages, (unsigned long long)s.unmappedPages);
    }
}

PmmBugKind pmmLastBug(void) {
    return pmmLastBugKind;
}

static Status pmmEarlyTableAlloc(uint64_t *outPhys) {
    return pmmEarlyAllocPages(1, outPhys);
}

void pmmInit(const BootInfo *bi) {
    pmmHhdmBaseValue = bi->hhdmBase;

    const BootMemRegion *regions =
        (const BootMemRegion *)(uintptr_t)(bi->hhdmBase + bi->memMapPhys);
    Status st = pmmMapScan(regions, bi->memMapCount, &pmmMap);
    if (st != STATUS_OK) {
        panic("pmm: memory map scan failed (status %d)", (int)st);
    }
    if (pmmMap.spanCount == 0) {
        panic("pmm: no managed memory found in the BootInfo map");
    }

    /* HHDM coverage check (D-080): every managed *region* must already be identity-mapped through
     * the HHDM, exactly as D-059 promises -- verified before anything is zeroed through it, since
     * the loader's own memmap.c admits it never checks this itself. Deliberately walks the raw
     * `regions[]` list, not pmmMap.spans: a span's order-10 alignment padding can cover physical
     * holes (e.g. the legacy 0xA0000-0x100000 VGA/BIOS range) that never appeared in the BootInfo
     * map at all and so carry no HHDM-mapping guarantee whatsoever. */
    uint64_t hhdmLimit = BOOTINFO_HHDM_SIZE;
    for (uint32_t i = 0; i < bi->memMapCount; i++) {
        const BootMemRegion *r = &regions[i];
        if (!pmmMapTypeIsManaged(r->type)) {
            continue;
        }
        uint64_t phys = r->base;
        uint64_t end = r->base + r->length;
        if (end > hhdmLimit) {
            end = hhdmLimit;
        }
        while (phys < end) {
            uint64_t va = bi->hhdmBase + phys;
            uint64_t pa, leafSize;
            Status lst = archEarlyLookup(va, &pa, &leafSize);
            if (lst != STATUS_OK || pa != phys) {
                panic("pmm: 0x%llx not HHDM-mapped (D-059 violated by the loader)",
                      (unsigned long long)phys);
            }
            /* A leaf can start before `phys` (phys isn't necessarily leaf-aligned on the first
             * iteration); advance by what's left of that leaf, not the whole leaf size. */
            uint64_t leafEnd = (va & ~(leafSize - 1)) + leafSize;
            phys += (leafEnd - va);
        }
    }

    pmmEarlyInit(&pmmMap, bi->hhdmBase);

    /* Map the Page array over every span, one 4 KiB leaf at a time -- every backing page and
     * every page table it needs comes from the bump allocator (never the loader's LOADER_RECLAIM
     * pool), so M2.3's later reclaim of LOADER_RECLAIM can never pull the rug out from under
     * live page-array mappings. */
    for (uint32_t i = 0; i < pmmMap.spanCount; i++) {
        uint64_t startVa = VM_PAGE_ARRAY_BASE + pmmMap.spans[i].startPfn * sizeof(Page);
        uint64_t endVa = VM_PAGE_ARRAY_BASE + pmmMap.spans[i].endPfn * sizeof(Page);
        for (uint64_t va = startVa; va < endVa; va += 4096) {
            uint64_t phys;
            if (pmmEarlyAllocPages(1, &phys) != STATUS_OK) {
                panic("pmm: not enough memory for the Page array");
            }
            pmmPageArrayPages++;
            Status mst = archEarlyMapPage(va, phys, pmmEarlyTableAlloc);
            if (mst != STATUS_OK) {
                panic("pmm: failed to map Page array VA 0x%llx (status %d)", (unsigned long long)va,
                      (int)mst);
            }
        }
    }

    pmmEarlySeal();
    pmmPageTablePages = pmmEarlyUsedPages() - pmmPageArrayPages;

    klogWrite(KLOG_INFO, "pmm",
              "Page array: %llu KiB (%llu KiB page tables), %u span(s), maxPfn=0x%llx",
              (unsigned long long)pmmPageArrayPages * 4, (unsigned long long)pmmPageTablePages * 4,
              pmmMap.spanCount, (unsigned long long)pmmMap.maxPfn);

    buddyZoneInit(&pmmZones[PMM_ZONE_DMA32], "DMA32", 0, PMM_ZONE_DMA32_END_PFN);
    buddyZoneInit(&pmmZones[PMM_ZONE_NORMAL], "NORMAL", PMM_ZONE_DMA32_END_PFN, pmmMap.maxPfn);
    pmmCacheInitAll();

    for (uint32_t i = 0; i < pmmMap.usableCount; i++) {
        uint64_t startPfn = pmmMap.usable[i].startPfn;
        uint64_t endPfn = pmmMap.usable[i].endPfn; /* shrunk in place by pmmEarlySeal() */
        if (startPfn < endPfn) {
            Status fst = pmmAddFreeRange(startPfn << 12, (endPfn - startPfn) << 12);
            if (fst != STATUS_OK) {
                panic("pmm: pmmAddFreeRange failed during init (status %d)", (int)fst);
            }
        }
    }

    pmmPrintMeminfo(); /* panics internally if the self-check fails */
}
