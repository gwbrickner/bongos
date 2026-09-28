/* Host tests for boot/common/bootheap.c: stage2's own [1 MiB, 4 GiB) bump allocator (D-107). */
#include "bootheap.h"
#include "framework/test.h"

TEST(bootHeapClipsToWindowAndDropsUnusable) {
    BootMemRegion regions[3] = {
        {0x0, 0x100000, BOOT_MEM_USABLE, 0},       /* entirely below 1 MiB: dropped */
        {0x80000, 0x200000, BOOT_MEM_RESERVED, 0}, /* wrong type: dropped */
        {0x100000, 0x2000, BOOT_MEM_USABLE, 0},    /* [1 MiB, 1 MiB + 8 KiB) */
    };
    BootHeap heap;
    ASSERT_EQ(bootHeapInit(&heap, regions, 3), BOOT_OK);
    ASSERT_EQ(heap.count, 1u);
    ASSERT_EQ(heap.regions[0].base, BOOT_HEAP_MIN);
    ASSERT_EQ(heap.regions[0].end, BOOT_HEAP_MIN + 0x2000ULL);
}

TEST(bootHeapClipsAtFourGiB) {
    BootMemRegion regions[1] = {
        {0xFFFFF000ULL, 0x200000ULL, BOOT_MEM_USABLE, 0}, /* straddles the 4 GiB line */
    };
    BootHeap heap;
    ASSERT_EQ(bootHeapInit(&heap, regions, 1), BOOT_OK);
    ASSERT_EQ(heap.count, 1u);
    ASSERT_EQ(heap.regions[0].end, BOOT_HEAP_MAX);
}

TEST(bootHeapAllocIsBumpOnlyFirstFit) {
    BootMemRegion regions[2] = {
        {0x100000, 0x3000, BOOT_MEM_USABLE, 0}, /* 3 pages */
        {0x200000, 0x1000, BOOT_MEM_USABLE, 0}, /* 1 page */
    };
    BootHeap heap;
    ASSERT_EQ(bootHeapInit(&heap, regions, 2), BOOT_OK);

    uint64_t p1 = 0, p2 = 0, p3 = 0;
    ASSERT_EQ(bootHeapAllocPages(&heap, 2, &p1), BOOT_OK);
    ASSERT_EQ(p1, 0x100000ULL);
    ASSERT_EQ(bootHeapAllocPages(&heap, 1, &p2), BOOT_OK);
    ASSERT_EQ(p2, 0x102000ULL); /* still fits in the first region's remaining 1 page */
    /* First region is now exhausted; a 1-page request falls through to the second region. */
    ASSERT_EQ(bootHeapAllocPages(&heap, 1, &p3), BOOT_OK);
    ASSERT_EQ(p3, 0x200000ULL);
}

TEST(bootHeapAllocFailsWhenNothingFits) {
    BootMemRegion regions[1] = {
        {0x100000, 0x1000, BOOT_MEM_USABLE, 0}, /* 1 page */
    };
    BootHeap heap;
    ASSERT_EQ(bootHeapInit(&heap, regions, 1), BOOT_OK);
    uint64_t phys = 0;
    ASSERT_EQ(bootHeapAllocPages(&heap, 2, &phys), BOOT_ERR_NO_MEMORY);
}

TEST(bootHeapInitRejectsTooManyRegions) {
    BootMemRegion regions[BOOT_HEAP_MAX_REGIONS + 1];
    for (uint32_t i = 0; i < BOOT_HEAP_MAX_REGIONS + 1; i++) {
        regions[i].base = BOOT_HEAP_MIN + (uint64_t)i * 0x10000ULL;
        regions[i].length = 0x1000;
        regions[i].type = BOOT_MEM_USABLE;
        regions[i].reserved = 0;
    }
    BootHeap heap;
    ASSERT_EQ(bootHeapInit(&heap, regions, BOOT_HEAP_MAX_REGIONS + 1), BOOT_ERR_MEMMAP_CAPACITY);
}
