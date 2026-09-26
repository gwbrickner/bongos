/* Host tests for kernel/mm/kva.c (D-088, ROADMAP M2.3): the pure KVA extent allocator. */
#include "framework/test.h"
#include "kva-internal.h"

TEST(kvaInitStartsAsOneExtent) {
    KvaState st;
    kvaStateInit(&st, 0x1000, 0x100000);
    ASSERT_EQ(st.count, 1u);
    ASSERT_EQ(st.extents[0].start, (uint64_t)0x1000);
    ASSERT_EQ(st.extents[0].end, (uint64_t)0x100000);
}

TEST(kvaAllocReservesGuardPages) {
    KvaState st;
    kvaStateInit(&st, 0, 0x100000);
    uint64_t va;
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va) == STATUS_OK);
    ASSERT_EQ(va, KVA_GUARD_SIZE);
    /* The extent shrinks from the front by size + 2 guards, leaving the rest free. */
    ASSERT_EQ(st.count, 1u);
    ASSERT_EQ(st.extents[0].start, (uint64_t)(0x1000 + 2 * KVA_GUARD_SIZE));
}

TEST(kvaAllocThenFreeRoundTrip) {
    KvaState st;
    kvaStateInit(&st, 0, 0x100000);
    uint64_t va;
    ASSERT_TRUE(kvaAlloc(&st, 0x2000, &va) == STATUS_OK);
    ASSERT_TRUE(kvaFree(&st, va, 0x2000) == STATUS_OK);
    ASSERT_EQ(st.count, 1u);
    ASSERT_EQ(st.extents[0].start, (uint64_t)0);
    ASSERT_EQ(st.extents[0].end, (uint64_t)0x100000);
}

TEST(kvaFreeCoalescesBothNeighbors) {
    KvaState st;
    kvaStateInit(&st, 0, 0x100000);
    uint64_t va1, va2, va3;
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va1) == STATUS_OK);
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va2) == STATUS_OK);
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va3) == STATUS_OK);
    ASSERT_TRUE(kvaFree(&st, va1, 0x1000) == STATUS_OK);
    ASSERT_TRUE(kvaFree(&st, va3, 0x1000) == STATUS_OK);
    /* va1's freed range is isolated (va2 is still allocated on its right); va3's freed range
     * borders the untouched remainder past it and merges into that immediately -- two extents. */
    ASSERT_EQ(st.count, 2u);
    ASSERT_TRUE(kvaFree(&st, va2, 0x1000) == STATUS_OK); /* merges all three back into one */
    ASSERT_EQ(st.count, 1u);
    ASSERT_EQ(st.extents[0].start, (uint64_t)0);
    ASSERT_EQ(st.extents[0].end, (uint64_t)0x100000);
}

TEST(kvaAllocFailsWhenExhausted) {
    KvaState st;
    kvaStateInit(&st, 0, 0x1000 + 2 * KVA_GUARD_SIZE);
    uint64_t va;
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va) == STATUS_OK);
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va) == STATUS_ERR_NO_MEMORY);
}

TEST(kvaAllocRejectsBadSize) {
    KvaState st;
    kvaStateInit(&st, 0, 0x100000);
    uint64_t va;
    ASSERT_TRUE(kvaAlloc(&st, 0, &va) == STATUS_ERR_INVALID);
    ASSERT_TRUE(kvaAlloc(&st, 0x123, &va) == STATUS_ERR_INVALID); /* not page-aligned */
}

TEST(kvaFreeDetectsDoubleFree) {
    KvaState st;
    kvaStateInit(&st, 0, 0x100000);
    uint64_t va;
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va) == STATUS_OK);
    ASSERT_TRUE(kvaFree(&st, va, 0x1000) == STATUS_OK);
    ASSERT_TRUE(kvaFree(&st, va, 0x1000) == STATUS_ERR_INVALID); /* already free */
}

TEST(kvaFreeDetectsOverlapWithFreeExtent) {
    KvaState st;
    kvaStateInit(&st, 0, 0x100000);
    uint64_t va1;
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va1) == STATUS_OK);
    ASSERT_TRUE(kvaFree(&st, va1, 0x1000) == STATUS_OK);
    /* va1's range is free again; a botched double free that overlaps it without being an exact
     * repeat (a bad size, not just a bad address) must still be rejected. */
    ASSERT_TRUE(kvaFree(&st, va1, 0x2000) == STATUS_ERR_INVALID);
}

TEST(kvaFirstFitPicksLowestAddress) {
    KvaState st;
    kvaStateInit(&st, 0, 0x100000);
    uint64_t a, b, c;
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &a) == STATUS_OK);
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &b) == STATUS_OK);
    ASSERT_TRUE(kvaFree(&st, a, 0x1000) == STATUS_OK);
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &c) == STATUS_OK);
    /* The freed low extent is reused before extending further into the untouched remainder. */
    ASSERT_EQ(c, a);
    ASSERT_TRUE(b != a); /* sanity: b really did land somewhere else */
}

TEST(kvaFreeRejectsOutOfBounds) {
    KvaState st;
    kvaStateInit(&st, 0x10000, 0x20000);
    /* A VA never handed out by this state (its own [base, end) is [0x10000, 0x20000)) must be
     * rejected outright, not silently added to the free list as if it belonged here. */
    ASSERT_TRUE(kvaFree(&st, 0x1000, 0x1000) == STATUS_ERR_INVALID);
    ASSERT_TRUE(kvaFree(&st, 0x30000, 0x1000) == STATUS_ERR_INVALID);
}

/* D-098 superseded the old "kvaFreeFailsWhenExtentTableFull" test: that test relied on
 * legitimately reaching a full, maximally-fragmented extent table by way of KVA_MAX_EXTENTS+1
 * live reservations, a state kvaAlloc()'s new liveCount admission cap (KVA_MAX_EXTENTS-1 = 511
 * live reservations) makes unreachable through any well-behaved caller -- see kva-internal.h's
 * kvaFree() contract comment for why that cap is enough to prove the table can never actually
 * fill up on a legitimate free. These two tests replace it: one confirms the cap itself rejects
 * a kvaAlloc() purely on live-reservation count (plenty of address space remains), the other
 * confirms that freeing under the heaviest fragmentation the cap allows never fails. */
TEST(kvaAllocRejectsAtLiveCap) {
    KvaState st;
    uint32_t cap = KVA_MAX_EXTENTS - 1;
    uint64_t pageStride = 0x1000 + 2 * KVA_GUARD_SIZE;
    kvaStateInit(&st, 0, (uint64_t)(cap + 4) * pageStride); /* generous address-space headroom */

    uint64_t va, firstVa = 0;
    for (uint32_t i = 0; i < cap; i++) {
        ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va) == STATUS_OK);
        if (i == 0) {
            firstVa = va;
        }
    }
    ASSERT_EQ(st.liveCount, cap);
    /* Rejected purely by the admission cap, not by address-space exhaustion (there's a whole
     * untouched extent left at the top of the range). */
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va) == STATUS_ERR_NO_MEMORY);

    /* Freeing one makes room again. */
    ASSERT_TRUE(kvaFree(&st, firstVa, 0x1000) == STATUS_OK);
    ASSERT_EQ(st.liveCount, cap - 1);
    ASSERT_TRUE(kvaAlloc(&st, 0x1000, &va) == STATUS_OK);
    ASSERT_EQ(st.liveCount, cap);
}

TEST(kvaFreeNeverFailsUnderMaxFragmentation) {
    KvaState st;
    /* Pair each "target" allocation with a "spacer" that stays allocated, boxing every target in
     * on both sides so freeing it can never coalesce -- the same construction the old test used,
     * scaled to what the liveCount cap actually allows (511 live reservations total, so at most
     * 255 target/spacer pairs). */
    uint32_t pairs = (KVA_MAX_EXTENTS - 1) / 2;
    uint64_t pageStride = 0x1000 + 2 * KVA_GUARD_SIZE;
    kvaStateInit(&st, 0, (uint64_t)(2 * pairs + 4) * pageStride);

    uint64_t targets[256];
    for (uint32_t i = 0; i < pairs; i++) {
        ASSERT_TRUE(kvaAlloc(&st, 0x1000, &targets[i]) == STATUS_OK);
        uint64_t spacerVa;
        ASSERT_TRUE(kvaAlloc(&st, 0x1000, &spacerVa) == STATUS_OK);
    }
    ASSERT_EQ(st.liveCount, 2 * pairs);

    /* Every target is isolated by its own spacer -- freeing all of them produces `pairs` isolated
     * free extents, well within KVA_MAX_EXTENTS, and every single free must succeed. */
    for (uint32_t i = 0; i < pairs; i++) {
        ASSERT_TRUE(kvaFree(&st, targets[i], 0x1000) == STATUS_OK);
    }
    ASSERT_EQ(st.count, (uint32_t)(pairs + 1)); /* pairs isolated gaps + the untouched tail */
}
