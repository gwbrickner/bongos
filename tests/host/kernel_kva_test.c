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
