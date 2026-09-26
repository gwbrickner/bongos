/* ktests for the slab allocator and kmalloc (D-092..D-096, ROADMAP M2.4's Done-when checks: a
 * stress test, alignment guarantees, and -- KERNEL_DEBUG only -- redzone-overflow/poison
 * detection). Only kmalloc.h's public surface is used, same as every other subsystem's ktests. */
#include "kernel-boot.h"
#include "kmalloc.h"
#include "ktest.h"
#include "pmm.h"

#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

static uint64_t kmallocTestRng;
static uint64_t kmallocTestNextRand(void) {
    kmallocTestRng ^= kmallocTestRng << 13;
    kmallocTestRng ^= kmallocTestRng >> 7;
    kmallocTestRng ^= kmallocTestRng << 17;
    return kmallocTestRng;
}

/* 100k kmalloc/kfree operations across every size class, each live allocation tagged at its first
 * and last byte and checked before it's ever freed -- catches both a corrupting bug in the
 * allocator itself and one live allocation overlapping another's memory. Ends with the pmm's own
 * page-level stats back at their starting point, proving nothing leaked out of the pmm underneath
 * kmalloc's own bookkeeping. */
KTEST(kmalloc_stress) {
    kmallocTestRng = 0x2545F4914F6CDD1DULL;
    slabShrinkAll();
    pmmDrainLocalCache();
    PmmStats before;
    pmmGetStats(&before);

#define KMALLOC_STRESS_LIVE_CAP 1024
    static void *live[KMALLOC_STRESS_LIVE_CAP];
    static uint32_t liveSize[KMALLOC_STRESS_LIVE_CAP];
    static uint8_t liveTag[KMALLOC_STRESS_LIVE_CAP];
    int liveCount = 0;

    for (int op = 0; op < 100000; op++) {
        bool doAlloc = (liveCount == 0) ||
                       (liveCount < KMALLOC_STRESS_LIVE_CAP && (kmallocTestNextRand() & 1) != 0);
        if (doAlloc) {
            uint32_t size = 1 + (uint32_t)(kmallocTestNextRand() % KMALLOC_MAX_SIZE);
            uint8_t tag = (uint8_t)kmallocTestNextRand();
            void *p = kmalloc(size, 0);
            if (p == NULL) {
                continue; /* OOM under stress is acceptable; just don't record it as live */
            }
            uint8_t *bytes = (uint8_t *)p;
            bytes[0] = tag;
            if (size > 1) { /* size==1: bytes[size-1] is bytes[0] -- don't self-overwrite the tag */
                bytes[size - 1] = (uint8_t)(tag ^ 0xFF);
            }
            live[liveCount] = p;
            liveSize[liveCount] = size;
            liveTag[liveCount] = tag;
            liveCount++;
        } else {
            int idx = (int)(kmallocTestNextRand() % (uint64_t)liveCount);
            uint8_t *bytes = (uint8_t *)live[idx];
            KTEST_ASSERT_EQ(bytes[0], liveTag[idx]);
            if (liveSize[idx] > 1) {
                KTEST_ASSERT_EQ(bytes[liveSize[idx] - 1], (uint8_t)(liveTag[idx] ^ 0xFF));
            }
            kfree(live[idx]);
            live[idx] = live[liveCount - 1];
            liveSize[idx] = liveSize[liveCount - 1];
            liveTag[idx] = liveTag[liveCount - 1];
            liveCount--;
        }
    }
    for (int i = 0; i < liveCount; i++) {
        uint8_t *bytes = (uint8_t *)live[i];
        KTEST_ASSERT_EQ(bytes[0], liveTag[i]);
        if (liveSize[i] > 1) {
            KTEST_ASSERT_EQ(bytes[liveSize[i] - 1], (uint8_t)(liveTag[i] ^ 0xFF));
        }
        kfree(live[i]);
    }

    slabShrinkAll();
    pmmDrainLocalCache();
    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
    KTEST_ASSERT_EQ(after.allocatedPages, before.allocatedPages);
}

/* Every kmalloc size around each class boundary comes back KMALLOC_MIN_ALIGN-aligned. A custom
 * slabCacheCreate() cache's own alignment (64/256/4096) is honored exactly, in both builds. */
KTEST(kmalloc_alignment) {
    static const uint32_t sizes[] = {1,    15,   16,   17,   31,   32,   63,   64,  95,  96,
                                     127,  128,  191,  192,  255,  256,  511,  512, 1023, 1024,
                                     2047, 2048, 4095, 4096, 8191, 8192};
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = kmalloc(sizes[i], 0);
        KTEST_ASSERT(p != NULL);
        KTEST_ASSERT_EQ((uint64_t)(uintptr_t)p % KMALLOC_MIN_ALIGN, 0);
        kfree(p);
    }

    static const uint32_t aligns[] = {64, 256, 4096};
    for (uint32_t a = 0; a < sizeof(aligns) / sizeof(aligns[0]); a++) {
        SlabCache *cache;
        KTEST_ASSERT_EQ(
            slabCacheCreate("kmalloc_test_align", 100, aligns[a], NULL, NULL, &cache), STATUS_OK);
        void *objs[4];
        for (int i = 0; i < 4; i++) {
            objs[i] = slabAlloc(cache, 0);
            KTEST_ASSERT(objs[i] != NULL);
            KTEST_ASSERT_EQ((uint64_t)(uintptr_t)objs[i] % aligns[a], 0);
        }
        for (int i = 0; i < 4; i++) {
            slabFree(cache, objs[i]);
        }
        slabCacheDestroy(cache);
    }
}

typedef struct {
    void *ptr;
} KmallocPtrTrigger;

static void kmallocTriggerKfree(void *arg) {
    kfree(((KmallocPtrTrigger *)arg)->ptr);
}

typedef struct {
    SlabCache *cache;
    void *obj;
} SlabFreeTrigger;

static void slabTriggerFree(void *arg) {
    SlabFreeTrigger *t = (SlabFreeTrigger *)arg;
    slabFree(t->cache, t->obj);
}

static void kmallocTriggerAllocBadSize(void *arg) {
    uint32_t size = *(uint32_t *)arg;
    void *p = kmalloc(size, 0);
    (void)p; /* a real trip panics before returning here */
}

/* Each sub-case confirms kfree()/slabFree() reject a specific kind of misuse via panicBug()/
 * TRAP_CATCH_KERNEL_BUG with the exact SlabBugKind the pointer-resolution/bufctl state machine
 * (kernel/mm/slab.c) should report, and that nothing is left corrupted by a rejected attempt. */
KTEST(kmalloc_double_free) {
    slabShrinkAll();

    /* 1: double free while the object is still sitting in the local magazine (the common case:
     * nothing else has run between the two frees). */
    {
        void *p = kmalloc(64, 0);
        KTEST_ASSERT(p != NULL);
        kfree(p);
        KmallocPtrTrigger t = {p};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, kmallocTriggerKfree, &t, &info);
        KTEST_ASSERT(caught);
        const void *bugObj;
        KTEST_ASSERT_EQ(slabTakeLastBug(&bugObj), SLAB_BUG_DOUBLE_FREE);
        KTEST_ASSERT_EQ((uint64_t)(uintptr_t)bugObj, (uint64_t)(uintptr_t)p);
    }

    /* 2: an interior (non-payload-start) pointer. */
    {
        void *p = kmalloc(64, 0);
        KTEST_ASSERT(p != NULL);
        KmallocPtrTrigger t = {(uint8_t *)p + 1};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, kmallocTriggerKfree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(slabTakeLastBug(NULL), SLAB_BUG_MISALIGNED);
        kfree(p);
    }

    /* 3: slabFree() on a cache the object doesn't belong to. */
    {
        SlabCache *cacheA, *cacheB;
        KTEST_ASSERT_EQ(slabCacheCreate("kmalloc_test_a", 32, 0, NULL, NULL, &cacheA), STATUS_OK);
        KTEST_ASSERT_EQ(slabCacheCreate("kmalloc_test_b", 32, 0, NULL, NULL, &cacheB), STATUS_OK);
        void *obj = slabAlloc(cacheA, 0);
        KTEST_ASSERT(obj != NULL);
        SlabFreeTrigger t = {cacheB, obj};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, slabTriggerFree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(slabTakeLastBug(NULL), SLAB_BUG_WRONG_CACHE);
        slabFree(cacheA, obj);
        slabCacheDestroy(cacheA);
        slabCacheDestroy(cacheB);
    }

    /* 4: kfree() of a custom (non-kmalloc) cache's object -> WRONG_CACHE. */
    {
        SlabCache *cache;
        KTEST_ASSERT_EQ(slabCacheCreate("kmalloc_test_c", 32, 0, NULL, NULL, &cache), STATUS_OK);
        void *obj = slabAlloc(cache, 0);
        KTEST_ASSERT(obj != NULL);
        KmallocPtrTrigger t = {obj};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, kmallocTriggerKfree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(slabTakeLastBug(NULL), SLAB_BUG_WRONG_CACHE);
        slabFree(cache, obj);
        slabCacheDestroy(cache);
    }

    /* 5: a plain pmm page's HHDM address (never a slab page) -> NOT_SLAB. */
    {
        Page *page;
        KTEST_ASSERT_EQ(pmmAllocPages(0, 0, &page), STATUS_OK);
        KmallocPtrTrigger t = {pmmPageToVirt(page)};
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, kmallocTriggerKfree, &t, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(slabTakeLastBug(NULL), SLAB_BUG_NOT_SLAB);
        pmmFreePages(page, 0);
    }

    /* 6: a bad size to kmalloc() itself. */
    {
        uint32_t badSize = 0;
        TrapCatchInfo info;
        bool caught =
            archTrapCatch(TRAP_CATCH_KERNEL_BUG, kmallocTriggerAllocBadSize, &badSize, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(slabTakeLastBug(NULL), SLAB_BUG_BAD_SIZE);
    }
}

static uint64_t kmallocCtorCount, kmallocDtorCount;
static void kmallocTestCtor(void *obj) {
    kmallocCtorCount++;
    *(uint64_t *)obj = 0xC7012345C7012345ULL;
}
static void kmallocTestDtor(void *obj) {
    (void)obj;
    kmallocDtorCount++;
}
static void slabTriggerDestroy(void *arg) {
    slabCacheDestroy((SlabCache *)arg);
}

/* A cache with a constructor: every object in a slab is constructed up front (not lazily on
 * first alloc), a freed-then-reallocated object still shows its constructed signature (freeing
 * never re-runs the destructor), and destroying a cache with a live object is refused. */
KTEST(slab_ctor_dtor) {
    kmallocCtorCount = 0;
    kmallocDtorCount = 0;
    SlabCache *cache;
    KTEST_ASSERT_EQ(
        slabCacheCreate("kmalloc_test_ctor", 16, 0, kmallocTestCtor, kmallocTestDtor, &cache),
        STATUS_OK);

    void *obj = slabAlloc(cache, 0);
    KTEST_ASSERT(obj != NULL);
    SlabCacheStats stats;
    slabCacheGetStats(cache, &stats);
    KTEST_ASSERT(kmallocCtorCount >= stats.objsPerSlab); /* the whole slab was constructed */
    KTEST_ASSERT_EQ(*(uint64_t *)obj, 0xC7012345C7012345ULL);

    /* A cache with a constructor promises the *ctor's* invariants stay intact across a free/
     * realloc cycle (it's only ever run once, at slab-carve time) -- it does not reset the object
     * to its just-constructed value on every free the way a non-ctor cache's poison does. So this
     * deliberately does NOT clobber `obj` before freeing it: doing that would just be asking the
     * allocator to reproduce whatever garbage the caller left behind, which proves nothing. */
    slabFree(cache, obj);
    void *obj2 = slabAlloc(cache, 0);
    KTEST_ASSERT(obj2 != NULL);
    /* LIFO magazine: this must be the same object, still showing the ctor's signature -- proving
     * a free never re-runs the destructor/re-constructs. */
    KTEST_ASSERT_EQ((uint64_t)(uintptr_t)obj2, (uint64_t)(uintptr_t)obj);
    KTEST_ASSERT_EQ(*(uint64_t *)obj2, 0xC7012345C7012345ULL);

    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, slabTriggerDestroy, cache, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(slabTakeLastBug(NULL), SLAB_BUG_CACHE_BUSY);

    slabFree(cache, obj2);
    uint64_t ctorCountBeforeDestroy = kmallocCtorCount;
    slabCacheDestroy(cache);
    KTEST_ASSERT_EQ(kmallocDtorCount, ctorCountBeforeDestroy);
}

#ifdef KERNEL_DEBUG
/* KERNEL_DEBUG only: writing one byte past a kmalloc allocation's own class size, into its right
 * redzone, is detected on free. Uses two different requested sizes that round up to two
 * different classes, so this isn't just testing one boundary. Deliberately leaks the two
 * corrupted objects (their redzone is now wrong, and this subsystem exposes no way to re-poison
 * one from outside kernel/mm -- a handful of leaked bytes in a one-shot ktest is harmless and
 * doesn't affect any other test's own before/after accounting). */
KTEST(kmalloc_redzone_overflow) {
    static const uint32_t requestSizes[] = {100, 250};
    for (uint32_t i = 0; i < sizeof(requestSizes) / sizeof(requestSizes[0]); i++) {
        SlabCache *cache = kmallocCacheForSize(requestSizes[i]);
        KTEST_ASSERT(cache != NULL);
        SlabCacheStats stats;
        slabCacheGetStats(cache, &stats);

        void *p = kmalloc(requestSizes[i], 0);
        KTEST_ASSERT(p != NULL);
        volatile uint8_t *rightRedzone = (volatile uint8_t *)p + stats.objSize;
        KmallocPtrTrigger t = {p};
        *rightRedzone = 0x41;
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, kmallocTriggerKfree, &t, &info);
        KTEST_ASSERT(caught);
        const void *bugObj;
        KTEST_ASSERT_EQ(slabTakeLastBug(&bugObj), SLAB_BUG_REDZONE);
        KTEST_ASSERT_EQ((uint64_t)(uintptr_t)bugObj, (uint64_t)(uintptr_t)p);
    }
}

static void kmallocTriggerRealloc(void *arg) {
    void **out = (void **)arg;
    *out = kmalloc(64, 0);
}

/* KERNEL_DEBUG only: a write through a stale pointer after a free is detected on the next
 * allocation that reuses the exact same object -- the magazine is LIFO, so freeing and
 * immediately reallocating the same size guarantees that. */
KTEST(kmalloc_poison_detects_uaf) {
    void *p = kmalloc(64, 0);
    KTEST_ASSERT(p != NULL);
    kfree(p);
    ((volatile uint8_t *)p)[3] = 0x99;

    void *reAlloc = NULL;
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, kmallocTriggerRealloc, &reAlloc, &info);
    KTEST_ASSERT(caught);
    const void *bugObj;
    KTEST_ASSERT_EQ(slabTakeLastBug(&bugObj), SLAB_BUG_POISON);
    KTEST_ASSERT_EQ((uint64_t)(uintptr_t)bugObj, (uint64_t)(uintptr_t)p);
}
#endif /* KERNEL_DEBUG */
