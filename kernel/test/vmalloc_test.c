/* ktests for vmalloc (D-092/D-097, ROADMAP M2.4's Done-when checks: a guard-page write faults,
 * plus map/free round-trip and misuse coverage). */
#include "kmalloc.h"
#include "ktest.h"
#include "pmm.h"
#include "vmalloc.h"
#include "vmm.h"

#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

static void vmallocReadTrigger(void *arg) {
    volatile const uint8_t *p = (volatile const uint8_t *)arg;
    volatile uint8_t v = *p;
    (void)v;
}
static void vmallocWriteTrigger(void *arg) {
    volatile uint8_t *p = (volatile uint8_t *)arg;
    *p = 0x42;
}

/* A vmalloc() allocation is bounded by an unmapped guard page on each side (vmmKvaAlloc()'s own
 * guarantee, D-088) -- writing one byte before the start or one byte past the end must fault, not
 * silently corrupt whatever VA happens to sit there. */
KTEST(vmalloc_guard_page_faults) {
    void *p = vmalloc(4096, 0);
    KTEST_ASSERT(p != NULL);
    uint64_t va = (uint64_t)(uintptr_t)p;

    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_VEC(14), vmallocWriteTrigger,
                                (void *)(uintptr_t)(va + 4096), &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.cr2, va + 4096);

    caught =
        archTrapCatch(TRAP_CATCH_VEC(14), vmallocReadTrigger, (void *)(uintptr_t)(va - 1), &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.cr2, va - 1);

    vfree(p);
}

typedef struct {
    void *ptr;
} VfreeTrigger;
static void vfreeTrigger(void *arg) {
    vfree(((VfreeTrigger *)arg)->ptr);
}

/* Several sizes crossing a page boundary in different ways: every page gets touched (proving it's
 * really mapped and writable), and after vfree() the whole range is unmapped -- a real #PF, not
 * just a Status code -- with the pmm's own page-level stats (not just vmalloc's own counters,
 * which change on every call whether or not a frame was actually freed) back where they started.
 * VMALLOC_ZERO memory reads back as zero. A double vfree() and an interior vfree() are both
 * rejected. */
KTEST(vmalloc_map_free_no_leak) {
    /* Warm up first: the very first mapping into a fresh stretch of the kernel virtual area can
     * need new page-table pages that M2.3's vmm never frees back (vmmUnmapKernel()'s documented
     * contract) -- do one throwaway round trip, larger than anything this test allocates below,
     * so that cost lands before the real before/after snapshot instead of polluting it. First-fit
     * KVA allocation means every smaller allocation below reuses this same freed stretch. */
    void *warm = vmalloc(8 * 1024 * 1024, 0);
    KTEST_ASSERT(warm != NULL);
    vfree(warm);

    slabShrinkAll();
    pmmDrainLocalCache();
    PmmStats pmmBefore;
    pmmGetStats(&pmmBefore);
    VmallocStats before;
    vmallocGetStats(&before);

    static const uint64_t sizes[] = {1, 4096, 4097, 65536, 1048577};
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = vmalloc(sizes[i], VMALLOC_ZERO);
        KTEST_ASSERT(p != NULL);
        uint64_t pages = (sizes[i] + 4095) / 4096;

        volatile uint8_t *bytes = (volatile uint8_t *)p;
        for (uint64_t pg = 0; pg < pages; pg++) {
            KTEST_ASSERT_EQ(bytes[pg * 4096], 0); /* VMALLOC_ZERO */
            bytes[pg * 4096] = (uint8_t)(0x10 + pg);
        }
        for (uint64_t pg = 0; pg < pages; pg++) {
            KTEST_ASSERT_EQ(bytes[pg * 4096], (uint8_t)(0x10 + pg));
        }

        uint64_t va = (uint64_t)(uintptr_t)p;
        vfree(p);

        uint64_t pa;
        VmmFlags outFlags;
        KTEST_ASSERT_EQ(vmmLookupKernel(va, &pa, &outFlags), STATUS_ERR_NOT_FOUND);

        VfreeTrigger t = {p};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, vfreeTrigger, &t, &info);
        KTEST_ASSERT(caught); /* double vfree() */
        VmallocBugKind bug = vmallocTakeLastBug();
        KTEST_ASSERT_EQ(bug, VMALLOC_BUG_NOT_MAPPED);
    }

    slabShrinkAll();
    pmmDrainLocalCache();
    PmmStats pmmAfter;
    pmmGetStats(&pmmAfter);
    KTEST_ASSERT_EQ(pmmAfter.freePages, pmmBefore.freePages);
    KTEST_ASSERT_EQ(pmmAfter.allocatedPages, pmmBefore.allocatedPages);

    VmallocStats after;
    vmallocGetStats(&after);
    KTEST_ASSERT_EQ(after.areas, before.areas);
    KTEST_ASSERT_EQ(after.pages, before.pages);
}

/* vfree() of an interior page (not the area's first page) is rejected without unmapping anything
 * -- the whole area must still be usable afterward. */
KTEST(vmalloc_interior_free_rejected) {
    void *p = vmalloc(3 * 4096, 0);
    KTEST_ASSERT(p != NULL);
    void *interior = (uint8_t *)p + 4096;

    VfreeTrigger t = {interior};
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, vfreeTrigger, &t, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(vmallocTakeLastBug(), VMALLOC_BUG_NOT_HEAD);

    volatile uint8_t *bytes = (volatile uint8_t *)p;
    bytes[0] = 1;
    bytes[4096] = 2;
    bytes[2 * 4096] = 3;
    vfree(p);
}

/* vfree() of a page that's genuinely mapped in the kernel virtual area, but never went through
 * vmalloc() (so it never got PAGE_F_VMALLOC stamped) -- built directly on vmmKvaAlloc()/
 * vmmMapKernel() the way a hypothetical MMIO or thread-stack mapping would be, to prove vfree()
 * distinguishes "some KVA mapping" from "one of ours". */
KTEST(vmalloc_not_vmalloc_page_rejected) {
    Page *page;
    KTEST_ASSERT_EQ(pmmAllocPages(0, 0, &page), STATUS_OK);
    uint64_t pa = pmmPageToPhys(page);
    uint64_t va;
    KTEST_ASSERT_EQ(vmmKvaAlloc(4096, &va), STATUS_OK);
    KTEST_ASSERT_EQ(vmmMapKernel(va, pa, 4096, VMM_WRITE), STATUS_OK);

    VfreeTrigger t = {(void *)(uintptr_t)va};
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, vfreeTrigger, &t, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(vmallocTakeLastBug(), VMALLOC_BUG_NOT_VMALLOC);

    KTEST_ASSERT_EQ(vmmUnmapKernel(va, 4096), STATUS_OK);
    vmmKvaFree(va, 4096);
    pmmFreePages(page, 0);
}

/* A vmalloc() that runs out of pmm memory partway through mapping its pages must unwind every
 * page it already mapped, not just return NULL and leak them (kernel/mm/vmalloc.c's
 * vmallocUnwind()) -- proven here by deliberately exhausting the pmm down to exactly 3 free
 * pages and asking for 8. */
KTEST(vmalloc_oom_rollback) {
    /* Warm the VA region's page tables *and* leave the vmalloc-area slab cache's own backing slab
     * alive (deliberately no slabShrinkAll() here) -- the OOM attempt below then reuses both
     * instead of growing either, so the pmm accounting below comes out exact, not just close. */
    void *warm = vmalloc(64 * 4096, 0);
    KTEST_ASSERT(warm != NULL);
    vfree(warm);
    pmmDrainLocalCache();

    /* Drain every free page in the machine, across every order, chaining each held block through
     * its own first two qwords (next pointer, order) so no side array is needed. */
    Page *held = NULL;
    for (int order = PMM_MAX_ORDER; order >= 0; order--) {
        Page *page;
        while (pmmAllocPages((uint32_t)order, 0, &page) == STATUS_OK) {
            uint64_t *link = (uint64_t *)pmmPageToVirt(page);
            link[0] = (uint64_t)(uintptr_t)held;
            link[1] = (uint64_t)order;
            held = page;
        }
    }

    /* Free back exactly 3 order-0 pages -- the last ones taken, so guaranteed order 0 -- leaving
     * just enough free memory for an 8-page vmalloc() to get partway through, then fail. */
    for (int i = 0; i < 3; i++) {
        KTEST_ASSERT(held != NULL);
        uint64_t *link = (uint64_t *)pmmPageToVirt(held);
        Page *next = (Page *)(uintptr_t)link[0];
        KTEST_ASSERT_EQ(link[1], 0);
        pmmFreePages(held, 0);
        held = next;
    }

    PmmStats pmmBefore;
    pmmGetStats(&pmmBefore);
    VmallocStats vBefore;
    vmallocGetStats(&vBefore);

    void *p = vmalloc(8 * 4096, 0);
    KTEST_ASSERT(p == NULL);

    PmmStats pmmAfter;
    pmmGetStats(&pmmAfter);
    VmallocStats vAfter;
    vmallocGetStats(&vAfter);
    KTEST_ASSERT_EQ(pmmAfter.freePages, pmmBefore.freePages);
    KTEST_ASSERT_EQ(pmmAfter.allocatedPages, pmmBefore.allocatedPages);
    KTEST_ASSERT_EQ(vAfter.areas, vBefore.areas);
    KTEST_ASSERT_EQ(vAfter.pages, vBefore.pages);

    /* Release everything else held, then confirm a normal vmalloc() works again. */
    while (held != NULL) {
        uint64_t *link = (uint64_t *)pmmPageToVirt(held);
        Page *next = (Page *)(uintptr_t)link[0];
        pmmFreePages(held, (uint32_t)link[1]);
        held = next;
    }
    void *q = vmalloc(4096, 0);
    KTEST_ASSERT(q != NULL);
    vfree(q);
}
