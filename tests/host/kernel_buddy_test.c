/* Host tests for kernel/mm/buddy.c (D-081, ROADMAP M2.2): the pure buddy-allocator core, checked
 * against a simple reference bitmap model under random alloc/free traffic. */
#include "framework/test.h"
#include "pmm-internal.h"

#include <stdint.h>
#include <stdlib.h>

#define TEST_FRAMES 16384 /* 16384 * 64 B = 1 MiB of Page entries */

static Page testPageArray[TEST_FRAMES];
uintptr_t hostPageArrayBase; /* page.h's HOSTED override */

static void resetPageArray(void) {
    for (int i = 0; i < TEST_FRAMES; i++) {
        testPageArray[i] = (Page){0}; /* PAGE_STATE_RESERVED */
    }
    hostPageArrayBase = (uintptr_t)testPageArray;
}

/* Frees [startPfn, endPfn) into `zone` as the largest aligned blocks it decomposes into --
 * mirrors what kernel/mm/pmm.c's pmmAddFreeRange() does over real BootInfo ranges. */
static void freeRangeAsBlocks(PmmZone *zone, uint64_t startPfn, uint64_t endPfn) {
    uint64_t pfn = startPfn;
    while (pfn < endPfn) {
        uint32_t order = pfn == 0 ? PMM_MAX_ORDER : (uint32_t)__builtin_ctzll(pfn);
        if (order > PMM_MAX_ORDER) {
            order = PMM_MAX_ORDER;
        }
        while (((uint64_t)1 << order) > endPfn - pfn) {
            order--;
        }
        for (uint64_t p = pfn; p < pfn + ((uint64_t)1 << order); p++) {
            testPageArray[p].state = PAGE_STATE_TAIL;
        }
        buddyFreeBlock(zone, pfn, order);
        pfn += (uint64_t)1 << order;
    }
}

TEST(buddyInitStartsEmpty) {
    PmmZone zone;
    buddyZoneInit(&zone, "TEST", 0, TEST_FRAMES);
    for (uint32_t k = 0; k < PMM_ORDER_COUNT; k++) {
        ASSERT_EQ(zone.freeBlocks[k], 0u);
    }
    ASSERT_EQ(zone.freePages, 0u);
    ASSERT_EQ(zone.managedPages, 0u);
    uint64_t pfn;
    ASSERT_TRUE(!buddyAllocBlock(&zone, 0, &pfn));
}

TEST(buddyFreeThenAllocRoundTrip) {
    resetPageArray();
    PmmZone zone;
    buddyZoneInit(&zone, "TEST", 0, 1024);
    for (uint64_t p = 0; p < 1024; p++) {
        testPageArray[p].state = PAGE_STATE_TAIL;
    }
    buddyFreeBlock(&zone, 0, 10); /* one order-10 block covering the whole 1024-frame zone */
    ASSERT_EQ(zone.freeBlocks[10], 1u);
    ASSERT_EQ(zone.freePages, 1024u);

    uint64_t pfn;
    ASSERT_TRUE(buddyAllocBlock(&zone, 0, &pfn));
    ASSERT_EQ(pfn, 0u);
    ASSERT_EQ(testPageArray[0].state, PAGE_STATE_TAIL); /* caller (pmm.c) sets the real state */
    ASSERT_EQ(zone.freePages, 1023u);
    /* The order-10 block must have split all the way down: one free block at every order 0-9. */
    for (uint32_t k = 0; k < 10; k++) {
        ASSERT_EQ(zone.freeBlocks[k], 1u);
    }
    ASSERT_EQ(zone.freeBlocks[10], 0u);
}

TEST(buddyMergeAcrossOrders) {
    resetPageArray();
    PmmZone zone;
    buddyZoneInit(&zone, "TEST", 0, 1024);
    for (uint64_t p = 0; p < 4; p++) {
        testPageArray[p].state = PAGE_STATE_TAIL;
    }
    /* Free 4 individual order-0 frames, buddy pairs in an order that forces merging up to
     * order 2: (0,1) merge to order 1, (2,3) merge to order 1, then those two merge to order 2. */
    buddyFreeBlock(&zone, 0, 0);
    buddyFreeBlock(&zone, 1, 0);
    ASSERT_EQ(zone.freeBlocks[0], 0u);
    ASSERT_EQ(zone.freeBlocks[1], 1u);
    buddyFreeBlock(&zone, 3, 0);
    buddyFreeBlock(&zone, 2, 0);
    ASSERT_EQ(zone.freeBlocks[1], 0u);
    ASSERT_EQ(zone.freeBlocks[2], 1u);
    ASSERT_EQ(zone.freePages, 4u);

    uint64_t pfn;
    ASSERT_TRUE(buddyAllocBlock(&zone, 2, &pfn));
    ASSERT_EQ(pfn, 0u);
    ASSERT_EQ(zone.freePages, 0u);
}

TEST(buddyDoesNotMergePastZoneManagedRegion) {
    /* Only [0, 512) is ever freed to the zone; [512, 1024) stays PAGE_STATE_RESERVED (as if it
     * were a hole the pmm never handed over). A free of the top order-9 block at pfn 0 must not
     * merge into the unmanaged buddy at pfn 512, since that Page is RESERVED, not BUDDY. */
    resetPageArray();
    PmmZone zone;
    buddyZoneInit(&zone, "TEST", 0, 1024);
    for (uint64_t p = 0; p < 512; p++) {
        testPageArray[p].state = PAGE_STATE_TAIL;
    }
    buddyFreeBlock(&zone, 0, 9);
    ASSERT_EQ(zone.freeBlocks[9], 1u);
    ASSERT_EQ(zone.freeBlocks[10], 0u);
    ASSERT_EQ(testPageArray[512].state, PAGE_STATE_RESERVED);
}

/* Simple LCG so the test is deterministic and its seed is reproducible from a failure message. */
static uint64_t rngState = 0x2545F4914F6CDD1Dull;
static uint64_t nextRand(void) {
    rngState = rngState * 6364136223846793005ull + 1442695040888963407ull;
    return rngState >> 33;
}

TEST(buddyRandomStressMatchesReferenceModel) {
    resetPageArray();
    PmmZone zone;
    buddyZoneInit(&zone, "TEST", 0, TEST_FRAMES);
    freeRangeAsBlocks(&zone, 0, TEST_FRAMES);
    uint64_t totalManaged = zone.managedPages;
    ASSERT_EQ(totalManaged, (uint64_t)TEST_FRAMES);

    /* Reference model: which frames are currently allocated, per the bitmap; and the exact set of
     * live (pfn, order) blocks this test itself handed out, so it can free them back correctly. */
    static bool allocatedBitmap[TEST_FRAMES];
    for (int i = 0; i < TEST_FRAMES; i++) {
        allocatedBitmap[i] = false;
    }
    typedef struct {
        uint64_t pfn;
        uint32_t order;
    } LiveBlock;
    static LiveBlock live[4096];
    int liveCount = 0;

    for (int op = 0; op < 100000; op++) {
        bool doAlloc = (liveCount == 0) || (nextRand() % 2 == 0);
        if (doAlloc && liveCount < 4096) {
            uint32_t order = (uint32_t)(nextRand() % PMM_ORDER_COUNT);
            uint64_t pfn;
            bool ok = buddyAllocBlock(&zone, order, &pfn);
            if (ok) {
                ASSERT_EQ(pfn % ((uint64_t)1 << order), 0u);
                for (uint64_t p = pfn; p < pfn + ((uint64_t)1 << order); p++) {
                    ASSERT_TRUE(!allocatedBitmap[p]);
                    allocatedBitmap[p] = true;
                }
                live[liveCount].pfn = pfn;
                live[liveCount].order = order;
                liveCount++;
            }
        } else {
            int idx = (int)(nextRand() % (uint64_t)liveCount);
            LiveBlock b = live[idx];
            live[idx] = live[liveCount - 1];
            liveCount--;
            for (uint64_t p = b.pfn; p < b.pfn + ((uint64_t)1 << b.order); p++) {
                ASSERT_TRUE(allocatedBitmap[p]);
                allocatedBitmap[p] = false;
                testPageArray[p].state = PAGE_STATE_TAIL;
            }
            buddyFreeBlock(&zone, b.pfn, b.order);
        }

        uint64_t sum = 0;
        for (uint32_t k = 0; k < PMM_ORDER_COUNT; k++) {
            sum += zone.freeBlocks[k] << k;
        }
        ASSERT_EQ(sum, zone.freePages);
    }

    /* Free everything still outstanding and confirm the zone returns to exactly its starting
     * canonical form: one order-10 block per 1024-frame chunk (TEST_FRAMES is a multiple of
     * 1024), nothing at any other order. */
    for (int i = 0; i < liveCount; i++) {
        LiveBlock b = live[i];
        for (uint64_t p = b.pfn; p < b.pfn + ((uint64_t)1 << b.order); p++) {
            allocatedBitmap[p] = false;
            testPageArray[p].state = PAGE_STATE_TAIL;
        }
        buddyFreeBlock(&zone, b.pfn, b.order);
    }
    ASSERT_EQ(zone.freePages, totalManaged);
    ASSERT_EQ(zone.freeBlocks[10], (uint64_t)(TEST_FRAMES / 1024));
    for (uint32_t k = 0; k < PMM_MAX_ORDER; k++) {
        ASSERT_EQ(zone.freeBlocks[k], 0u);
    }
    for (int i = 0; i < TEST_FRAMES; i++) {
        ASSERT_TRUE(!allocatedBitmap[i]);
    }
}
