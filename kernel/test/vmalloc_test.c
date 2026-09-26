/* ktests for vmalloc (D-092/D-097, ROADMAP M2.4's Done-when checks: a guard-page write faults,
 * plus map/free round-trip and misuse coverage). */
#include "ktest.h"
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
    bool caught =
        archTrapCatch(TRAP_CATCH_VEC(14), vmallocWriteTrigger, (void *)(uintptr_t)(va + 4096),
                       &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.cr2, va + 4096);

    caught = archTrapCatch(TRAP_CATCH_VEC(14), vmallocReadTrigger, (void *)(uintptr_t)(va - 1),
                            &info);
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
 * just a Status code -- with the pmm/vmalloc stats back where they started. VMALLOC_ZERO memory
 * reads back as zero. A double vfree() and an interior vfree() are both rejected. */
KTEST(vmalloc_map_free_no_leak) {
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
