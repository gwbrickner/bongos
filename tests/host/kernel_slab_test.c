/* Host tests for kernel/mm/slab-core.c and slab-debug.c (D-093/D-096, ROADMAP M2.4): the slab
 * allocator's pure layout/bufctl/redzone/poison core. */
#include "framework/test.h"
#include "slab-internal.h"

#include <stdint.h>
#include <string.h>

TEST(slabClassIndexForSizeMatchesTable) {
    ASSERT_TRUE(slabClassIndexForSize(0) == UINT32_MAX);
    ASSERT_TRUE(slabClassIndexForSize(KMALLOC_MAX_SIZE + 1) == UINT32_MAX);
    for (uint32_t i = 0; i < SLAB_KMALLOC_CLASS_COUNT; i++) {
        uint32_t sz = slabKmallocClassSizes[i];
        ASSERT_EQ(slabClassIndexForSize(sz), i);
        ASSERT_EQ(slabClassIndexForSize(sz - 1), i);
        uint32_t prevMax = i == 0 ? 0 : slabKmallocClassSizes[i - 1];
        ASSERT_EQ(slabClassIndexForSize(prevMax + 1), i);
    }
    ASSERT_TRUE(slabClassIndexForSize(KMALLOC_MAX_SIZE) == SLAB_KMALLOC_CLASS_COUNT - 1);
}

/* Every kmalloc class, in both release and debug layouts, must produce a layout that: fits at
 * least one object per slab, never overlaps the header/bufctl region, never runs past the slab's
 * own byte budget, and keeps every slot aligned. */
static void checkLayoutInvariants(uint32_t objSize, uint32_t align, bool debug) {
    SlabLayout layout;
    slabComputeLayout(objSize, align, debug, &layout);

    ASSERT_TRUE(layout.objsPerSlab >= 1);
    ASSERT_TRUE(layout.objsPerSlab <= SLAB_MAX_OBJECTS);
    ASSERT_TRUE(layout.order <= SLAB_MAX_ORDER);
    uint32_t slabBytes = 4096u << layout.order;

    uint32_t minObjOffset =
        (uint32_t)sizeof(Slab) + layout.objsPerSlab * (uint32_t)sizeof(uint16_t);
    ASSERT_TRUE(layout.objOffset >= minObjOffset);
    /* Object 0's own left redzone lives in [objOffset-rzLeft, objOffset) -- it must not reach
     * back far enough to overlap the bufctl array, or a fresh carve's redzone fill corrupts the
     * free list it sits right next to (the exact bug this invariant is here to catch). */
    ASSERT_TRUE(layout.objOffset - layout.rzLeft >= minObjOffset);
    ASSERT_EQ(layout.objOffset % align, 0u);
    ASSERT_EQ(layout.stride % align, 0u);
    ASSERT_TRUE(layout.stride >= objSize);
    ASSERT_TRUE((uint64_t)layout.objOffset + (uint64_t)layout.objsPerSlab * layout.stride <=
                slabBytes);

    if (debug) {
        ASSERT_TRUE(layout.rzLeft >= SLAB_REDZONE_SIZE);
        ASSERT_TRUE(layout.rzLeft >= align);
        uint32_t rzRight = layout.stride - layout.rzLeft - objSize;
        ASSERT_TRUE(rzRight >= SLAB_REDZONE_SIZE);
    } else {
        ASSERT_EQ(layout.rzLeft, 0u);
    }
}

TEST(slabComputeLayoutInvariantsAllClasses) {
    for (uint32_t i = 0; i < SLAB_KMALLOC_CLASS_COUNT; i++) {
        checkLayoutInvariants(slabKmallocClassSizes[i], KMALLOC_MIN_ALIGN, false);
        checkLayoutInvariants(slabKmallocClassSizes[i], KMALLOC_MIN_ALIGN, true);
    }
    /* Non-default alignments, as a custom slabCacheCreate() cache would use. */
    checkLayoutInvariants(64, 64, false);
    checkLayoutInvariants(64, 64, true);
    checkLayoutInvariants(200, 256, false);
    checkLayoutInvariants(200, 256, true);
    checkLayoutInvariants(4000, 4096, true);
}

TEST(slabIndexForOffsetRejectsMisaligned) {
    SlabLayout layout = {.order = 0, .objsPerSlab = 10, .stride = 32, .objOffset = 64, .rzLeft = 0};
    ASSERT_EQ(slabIndexForOffset(&layout, 0), 0);
    ASSERT_EQ(slabIndexForOffset(&layout, 32), 1);
    ASSERT_EQ(slabIndexForOffset(&layout, 9 * 32), 9);
    ASSERT_TRUE(slabIndexForOffset(&layout, 10 * 32) < 0); /* past objsPerSlab */
    ASSERT_TRUE(slabIndexForOffset(&layout, 5) < 0);       /* not a stride multiple */
    ASSERT_TRUE(slabIndexForOffset(&layout, 32 + 1) < 0);
}

/* Backs a Slab header + its out-of-band bufctl array in a plain host buffer, wired up with a real
 * SlabCache so slabSlot()/slabBufctl() resolve exactly as they would in the kernel. */
typedef struct {
    SlabCache cache;
    Slab slab;
    uint16_t bufctlStorage[16]; /* immediately follows `slab` in the real layout; kept as a
                                 * separate field here only so the struct is self-documenting --
                                 * slabBufctl() computes its address from `&slab`, not this name */
} SlabFixture;

static void slabFixtureInit(SlabFixture *fx, uint16_t objCount) {
    memset(fx, 0, sizeof(*fx));
    fx->cache.layout.stride = 32;
    fx->slab.cache = &fx->cache;
    fx->slab.objCount = objCount;
    fx->slab.objBase = (uint64_t)(uintptr_t)fx->bufctlStorage + sizeof(fx->bufctlStorage);
    ASSERT_TRUE((char *)&fx->bufctlStorage == (char *)&fx->slab + sizeof(Slab));
}

TEST(slabCarveBuildsFreeList) {
    SlabFixture fx;
    slabFixtureInit(&fx, 5);
    slabCarve(&fx.slab);
    ASSERT_EQ(fx.slab.freeCount, 5u);
    ASSERT_EQ(fx.slab.freeHead, 0u);
    uint16_t *bufctl = slabBufctl(&fx.slab);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(bufctl[i], (uint16_t)(i + 1));
    }
    ASSERT_EQ(bufctl[4], (uint16_t)SLAB_BUFCTL_END);
}

TEST(slabFreeListPopPushRoundTrips) {
    SlabFixture fx;
    slabFixtureInit(&fx, 3);
    slabCarve(&fx.slab);

    uint16_t a = slabFreeListPop(&fx.slab);
    ASSERT_EQ(a, 0u);
    ASSERT_EQ(fx.slab.freeCount, 2u);
    uint16_t *bufctl = slabBufctl(&fx.slab);
    ASSERT_EQ(bufctl[a], (uint16_t)SLAB_BUFCTL_BUSY);

    uint16_t b = slabFreeListPop(&fx.slab);
    ASSERT_EQ(b, 1u);
    uint16_t c = slabFreeListPop(&fx.slab);
    ASSERT_EQ(c, 2u);
    ASSERT_EQ(fx.slab.freeCount, 0u);
    ASSERT_EQ(fx.slab.freeHead, (uint16_t)SLAB_BUFCTL_END);

    slabFreeListPush(&fx.slab, b);
    ASSERT_EQ(fx.slab.freeCount, 1u);
    ASSERT_EQ(fx.slab.freeHead, b);
    slabFreeListPush(&fx.slab, a);
    ASSERT_EQ(fx.slab.freeHead, a);
    ASSERT_EQ(bufctl[a], b);

    /* Pop order mirrors push order (LIFO), and every slot is independently addressable. */
    ASSERT_EQ(slabFreeListPop(&fx.slab), a);
    ASSERT_EQ(slabFreeListPop(&fx.slab), b);
    ASSERT_EQ(fx.slab.freeCount, 0u);
}

/* A plain host buffer standing in for one slab object's slot: rzLeft bytes of left redzone,
 * objSize bytes of payload, then enough right redzone to reach `stride`. */
#define SLAB_DEBUG_TEST_OBJ_SIZE 40u
#define SLAB_DEBUG_TEST_ALIGN    16u

static void makeDebugCache(SlabCache *cache, SlabObjFn ctor) {
    memset(cache, 0, sizeof(*cache));
    cache->objSize = SLAB_DEBUG_TEST_OBJ_SIZE;
    cache->ctor = ctor;
    slabComputeLayout(cache->objSize, SLAB_DEBUG_TEST_ALIGN, true, &cache->layout);
}

TEST(slabDebugCarveAndCheckRedzonesRoundTrip) {
    SlabCache cache;
    makeDebugCache(&cache, NULL);
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *slot = buf + cache.layout.rzLeft;
    ASSERT_TRUE(slot + cache.objSize + SLAB_REDZONE_SIZE <= buf + sizeof(buf));

    slabDebugCarveFill(&cache, slot);
    ASSERT_TRUE(slabDebugCheckRedzones(&cache, slot));
    ASSERT_TRUE(slabDebugCheckPoison(&cache, slot)); /* no ctor: freshly carved == poisoned */

    /* Corrupting one byte just past the payload (the start of the right redzone) must be
     * detected -- the exact scenario the redzone-overflow ktest exercises for real. */
    slot[cache.objSize] ^= 0xFF;
    ASSERT_TRUE(!slabDebugCheckRedzones(&cache, slot));
    slot[cache.objSize] ^= 0xFF; /* restore */
    ASSERT_TRUE(slabDebugCheckRedzones(&cache, slot));

    /* Same for one byte just before the payload (the last byte of the left redzone). */
    slot[-1] ^= 0xFF;
    ASSERT_TRUE(!slabDebugCheckRedzones(&cache, slot));
    slot[-1] ^= 0xFF;
    ASSERT_TRUE(slabDebugCheckRedzones(&cache, slot));
}

TEST(slabDebugPoisonDetectsWriteAfterFree) {
    SlabCache cache;
    makeDebugCache(&cache, NULL);
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *slot = buf + cache.layout.rzLeft;

    slabDebugCarveFill(&cache, slot);
    ASSERT_TRUE(slabDebugCheckPoison(&cache, slot));

    slot[3] = 0x42; /* simulated write through a stale pointer after a free */
    ASSERT_TRUE(!slabDebugCheckPoison(&cache, slot));

    slabDebugPoisonFree(&cache, slot); /* re-poisoning (what a real free does) fixes it */
    ASSERT_TRUE(slabDebugCheckPoison(&cache, slot));
}

static void noopCtor(void *obj) {
    (void)obj;
}

TEST(slabDebugCtorCacheNeverPoisonsPayload) {
    SlabCache cache;
    makeDebugCache(&cache, noopCtor);
    uint8_t buf[256];
    memset(buf, 0x77, sizeof(buf)); /* simulated constructed state */
    uint8_t *slot = buf + cache.layout.rzLeft;

    slabDebugCarveFill(&cache, slot); /* must fill redzones but leave the payload alone */
    ASSERT_EQ(slot[0], 0x77u);
    ASSERT_TRUE(slabDebugCheckRedzones(&cache, slot));
    ASSERT_TRUE(slabDebugCheckPoison(&cache, slot)); /* always "true": a ctor cache never poisons */

    slabDebugPoisonFree(&cache, slot); /* a no-op for a ctor cache */
    ASSERT_EQ(slot[0], 0x77u);         /* constructed state survived the "free" */
}
