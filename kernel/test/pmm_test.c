/* ktests for the physical memory manager (D-079..D-082, ROADMAP M2.2's four Done-when checks: an
 * alloc/free stress test, a no-leak check, zone correctness, and double-free detection). */
#include "kernel-boot.h"
#include "ktest.h"
#include "pmm.h"

#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

/* xorshift64*: fast, deterministic, and reproducible from this fixed seed if a failure needs to
 * be replayed by hand. */
static uint64_t pmmTestRng;
static uint64_t pmmTestNextRand(void) {
    pmmTestRng ^= pmmTestRng << 13;
    pmmTestRng ^= pmmTestRng >> 7;
    pmmTestRng ^= pmmTestRng << 17;
    return pmmTestRng;
}

KTEST(pmm_alloc_free_stress) {
    pmmTestRng = 0x9E3779B97F4A7C15ULL;
    pmmDrainLocalCache();
    PmmStats before;
    pmmGetStats(&before);

    /* Orders 0-5 (up to 128 KiB blocks): wide enough to exercise splitting/merging across
     * several levels while keeping 100k ops fast under TCG (a KERNEL_DEBUG build poisons every
     * freed block's full content). */
#define PMM_STRESS_MAX_ORDER 5
#define PMM_STRESS_LIVE_CAP  512
    static Page *live[PMM_STRESS_LIVE_CAP];
    static uint32_t liveOrder[PMM_STRESS_LIVE_CAP];
    static uint64_t liveTag[PMM_STRESS_LIVE_CAP];
    int liveCount = 0;

    for (int op = 0; op < 100000; op++) {
        bool doAlloc =
            (liveCount == 0) || (liveCount < PMM_STRESS_LIVE_CAP && (pmmTestNextRand() & 1) != 0);
        if (doAlloc) {
            uint32_t order = (uint32_t)(pmmTestNextRand() % (PMM_STRESS_MAX_ORDER + 1));
            PmmFlags flags = (pmmTestNextRand() & 4) != 0 ? PMM_FLAG_DMA32 : 0;
            Page *page;
            Status st = pmmAllocPages(order, flags, &page);
            if (st == STATUS_OK) {
                uint64_t phys = pmmPageToPhys(page);
                KTEST_ASSERT_EQ(phys % ((((uint64_t)1 << order)) << 12), 0);
                if (flags & PMM_FLAG_DMA32) {
                    KTEST_ASSERT(phys + ((((uint64_t)1 << order)) << 12) <= 0x100000000ULL);
                }
                uint64_t tag = pmmTestNextRand();
                *(volatile uint64_t *)pmmPageToVirt(page) = tag;
                live[liveCount] = page;
                liveOrder[liveCount] = order;
                liveTag[liveCount] = tag;
                liveCount++;
            } else {
                KTEST_ASSERT_EQ(st, STATUS_ERR_NO_MEMORY);
            }
        } else {
            int idx = (int)(pmmTestNextRand() % (uint64_t)liveCount);
            Page *page = live[idx];
            uint32_t order = liveOrder[idx];
            KTEST_ASSERT_EQ(*(volatile uint64_t *)pmmPageToVirt(page), liveTag[idx]);
            live[idx] = live[liveCount - 1];
            liveOrder[idx] = liveOrder[liveCount - 1];
            liveTag[idx] = liveTag[liveCount - 1];
            liveCount--;
            pmmFreePages(page, order);
        }
    }

    for (int i = 0; i < liveCount; i++) {
        pmmFreePages(live[i], liveOrder[i]);
    }
    pmmDrainLocalCache();

    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
    KTEST_ASSERT_EQ(after.allocatedPages, before.allocatedPages);
    KTEST_ASSERT_EQ(after.cachedPages, before.cachedPages);
}

/* 1000 order-0 allocations crosses the per-CPU cache's refill/drain thresholds (PMM_PCP_BATCH=32,
 * PMM_PCP_HIGH=192) several times over, mixed with a handful of larger orders -- then confirms a
 * full free restores every counter exactly, including the per-zone free-page totals. */
KTEST(pmm_no_leak) {
    pmmDrainLocalCache();
    PmmStats before;
    pmmGetStats(&before);

#define PMM_NOLEAK_CAP 1200
    static Page *pages[PMM_NOLEAK_CAP];
    static uint32_t orders[PMM_NOLEAK_CAP];
    int n = 0;

    for (int i = 0; i < 1000 && n < PMM_NOLEAK_CAP; i++) {
        Page *page;
        if (pmmAllocPages(0, 0, &page) != STATUS_OK) {
            break;
        }
        pages[n] = page;
        orders[n] = 0;
        n++;
    }
    for (uint32_t order = 1; order <= 6 && n < PMM_NOLEAK_CAP; order++) {
        Page *page;
        if (pmmAllocPages(order, 0, &page) == STATUS_OK) {
            pages[n] = page;
            orders[n] = order;
            n++;
        }
    }

    for (int i = 0; i < n; i++) {
        pmmFreePages(pages[i], orders[i]);
    }
    pmmDrainLocalCache();

    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
    KTEST_ASSERT_EQ(after.allocatedPages, before.allocatedPages);
    KTEST_ASSERT_EQ(after.cachedPages, before.cachedPages);
    for (uint32_t z = 0; z < PMM_ZONE_COUNT; z++) {
        KTEST_ASSERT_EQ(after.zoneFreePages[z], before.zoneFreePages[z]);
    }
}

KTEST(pmm_zone_correctness) {
    pmmDrainLocalCache();
    PmmStats before;
    pmmGetStats(&before);

    Page *pages[8];
    uint32_t orders[8];
    int n = 0;
    for (uint32_t order = 0; order <= 7 && n < 8; order++) {
        Page *page;
        if (pmmAllocPages(order, PMM_FLAG_DMA32, &page) == STATUS_OK) {
            uint64_t phys = pmmPageToPhys(page);
            /* Every DMA32-flagged block must lie entirely below 4 GiB, whatever zone layout this
             * machine actually has (the default 512 MiB test config has no NORMAL-zone memory at
             * all; test-full's extra-memory row does). */
            KTEST_ASSERT(phys + ((((uint64_t)1 << order)) << 12) <= 0x100000000ULL);
            pages[n] = page;
            orders[n] = order;
            n++;
        }
    }
    for (int i = 0; i < n; i++) {
        pmmFreePages(pages[i], orders[i]);
    }
    pmmDrainLocalCache();

    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(after.zoneFreePages[PMM_ZONE_DMA32], before.zoneFreePages[PMM_ZONE_DMA32]);
    KTEST_ASSERT_EQ(after.zoneFreePages[PMM_ZONE_NORMAL], before.zoneFreePages[PMM_ZONE_NORMAL]);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
}

typedef struct {
    Page *page;
    uint32_t order;
} PmmFreeTrigger;

static void pmmTriggerFree(void *arg) {
    PmmFreeTrigger *t = (PmmFreeTrigger *)arg;
    pmmFreePages(t->page, t->order);
}

/* Each sub-case allocates (or locates) a page in a known bad state for a free, then confirms
 * pmmFreePages() panics via panicBug()/TRAP_CATCH_KERNEL_BUG with the exact PmmBugKind the state
 * machine (kernel/mm/pmm.c's pmmValidateForFree) should report -- and that the pmm's total free
 * count is unaffected by every rejected attempt. */
KTEST(pmm_double_free) {
    pmmDrainLocalCache();
    PmmStats before;
    pmmGetStats(&before);

    /* 1: order-0, freed twice -- the second free finds it sitting in the per-CPU cache (PCP). */
    {
        Page *page;
        KTEST_ASSERT_EQ(pmmAllocPages(0, 0, &page), STATUS_OK);
        pmmFreePages(page, 0);
        PmmFreeTrigger t = {page, 0};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, pmmTriggerFree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_KERNEL_BUG);
        KTEST_ASSERT_EQ(pmmLastBug(), PMM_BUG_DOUBLE_FREE);
    }

    /* 2: order-3, freed twice -- the second free either finds a free BUDDY head directly, or (if
     * this block's pfn happened to merge into its buddy's, becoming an interior page) is caught
     * by the TAIL-state coverage probe instead. Both are PMM_BUG_DOUBLE_FREE. */
    {
        Page *page;
        KTEST_ASSERT_EQ(pmmAllocPages(3, 0, &page), STATUS_OK);
        pmmFreePages(page, 3);
        PmmFreeTrigger t = {page, 3};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, pmmTriggerFree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(pmmLastBug(), PMM_BUG_DOUBLE_FREE);
    }

    /* 3: a live order-2 block freed with the wrong order -> ORDER_MISMATCH, nothing mutated, so
     * the correct free right afterward succeeds cleanly. */
    {
        Page *page;
        KTEST_ASSERT_EQ(pmmAllocPages(2, 0, &page), STATUS_OK);
        PmmFreeTrigger t = {page, 1};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, pmmTriggerFree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(pmmLastBug(), PMM_BUG_ORDER_MISMATCH);
        pmmFreePages(page, 2);
    }

    /* 4: the interior (non-head) page of a still-live order-1 block -> NOT_HEAD. */
    {
        Page *page;
        KTEST_ASSERT_EQ(pmmAllocPages(1, 0, &page), STATUS_OK);
        Page *interior = pmmPhysToPage(pmmPageToPhys(page) + 4096);
        PmmFreeTrigger t = {interior, 0};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, pmmTriggerFree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(pmmLastBug(), PMM_BUG_NOT_HEAD);
        pmmFreePages(page, 1);
    }

    /* 5: a frame the pmm never owned (the kernel image itself, BOOT_MEM_KERNEL -- backed with a
     * Page entry, D-079, but never handed to the buddy allocator) -> RESERVED_FRAME. */
    {
        uint64_t kernelPhys = kernelBootInfo()->kernelPhysBase;
        Page *kernelPage = pmmPhysToPage(kernelPhys);
        KTEST_ASSERT(kernelPage != NULL);
        PmmFreeTrigger t = {kernelPage, 0};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, pmmTriggerFree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(pmmLastBug(), PMM_BUG_RESERVED_FRAME);
    }

    pmmDrainLocalCache();
    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
    KTEST_ASSERT_EQ(after.allocatedPages, before.allocatedPages);
}
