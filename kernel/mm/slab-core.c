/* Pure slab layout/bufctl core (D-093, ROADMAP M2.4): no klog, no panic, no arch/pmm calls --
 * host-tested directly by tests/host/kernel_slab_test.c. See slab-internal.h. */
#include "slab-internal.h"

#include <stddef.h>

const uint32_t slabKmallocClassSizes[SLAB_KMALLOC_CLASS_COUNT] = {16,  32,  64,   96,   128,  192,
                                                                  256, 512, 1024, 2048, 4096, 8192};

uint32_t slabClassIndexForSize(size_t size) {
    if (size == 0 || size > KMALLOC_MAX_SIZE) {
        return UINT32_MAX;
    }
    for (uint32_t i = 0; i < SLAB_KMALLOC_CLASS_COUNT; i++) {
        if (size <= slabKmallocClassSizes[i]) {
            return i;
        }
    }
    return UINT32_MAX; /* unreachable: the last class equals KMALLOC_MAX_SIZE */
}

static uint32_t alignUp32(uint32_t v, uint32_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

/* The smallest legal objOffset for `n` objects at `align`: past the header, past the whole
 * bufctl[n] array, AND past `rzLeft` -- object 0's own left redzone lives in
 * [objOffset-rzLeft, objOffset), which must not overlap the bufctl array a slot below it. Getting
 * this wrong once corrupted the bufctl array's free-list links with 0xBB redzone bytes for every
 * debug-build class whose bufctl array plus rzLeft didn't already happen to clear an alignment
 * boundary on its own (kmalloc-1024 first caught it, ROADMAP M2.4's kmalloc_stress ktest). */
static uint32_t slabMinObjOffset(uint32_t n, uint32_t rzLeft, uint32_t align) {
    return alignUp32((uint32_t)sizeof(Slab) + n * (uint32_t)sizeof(uint16_t) + rzLeft, align);
}

void slabComputeLayout(uint32_t objSize, uint32_t align, bool debug, SlabLayout *out) {
    uint32_t rzLeft = debug ? (align > SLAB_REDZONE_SIZE ? align : SLAB_REDZONE_SIZE) : 0;
    uint32_t rzRight = debug ? SLAB_REDZONE_SIZE : 0;
    uint32_t stride = alignUp32(rzLeft + objSize + rzRight, align);

    for (uint32_t order = 0; order <= SLAB_MAX_ORDER; order++) {
        uint32_t slabBytes = 4096u << order;
        uint32_t nMax = slabBytes / stride;
        if (nMax > SLAB_MAX_OBJECTS) {
            nMax = SLAB_MAX_OBJECTS;
        }
        uint32_t n = nMax;
        while (n > 0) {
            uint32_t objOffset = slabMinObjOffset(n, rzLeft, align);
            if ((uint64_t)objOffset + (uint64_t)n * stride <= slabBytes) {
                break;
            }
            n--;
        }
        if (n >= 1 && (n >= 4 || order == SLAB_MAX_ORDER)) {
            out->order = order;
            out->objsPerSlab = n;
            out->stride = stride;
            out->rzLeft = rzLeft;
            out->objOffset = slabMinObjOffset(n, rzLeft, align);
            return;
        }
    }
    /* Unreachable for any objSize <= KMALLOC_MAX_SIZE with align <= 4096: order SLAB_MAX_ORDER's
     * 32 KiB always fits at least one object of the largest class plus its header, so the loop
     * above always returns from the order == SLAB_MAX_ORDER iteration if nothing smaller already
     * qualified. Leaves `*out` unset rather than guess, so a violation of that would be loud
     * (uninitialized-read tooling, or a visibly wrong layout) instead of silently plausible. */
}

int32_t slabIndexForOffset(const SlabLayout *layout, uint64_t off) {
    if (layout->stride == 0 || off % layout->stride != 0) {
        return -1;
    }
    uint64_t index = off / layout->stride;
    if (index >= layout->objsPerSlab) {
        return -1;
    }
    return (int32_t)index;
}

void slabCarve(Slab *slab) {
    uint16_t n = slab->objCount;
    uint16_t *bufctl = slabBufctl(slab);
    for (uint16_t i = 0; i < n; i++) {
        bufctl[i] = (uint16_t)((i + 1 < n) ? (i + 1) : SLAB_BUFCTL_END);
    }
    slab->freeHead = n > 0 ? 0 : SLAB_BUFCTL_END;
    slab->freeCount = n;
}

uint16_t slabFreeListPop(Slab *slab) {
    uint16_t *bufctl = slabBufctl(slab);
    uint16_t idx = slab->freeHead;
    slab->freeHead = bufctl[idx];
    bufctl[idx] = SLAB_BUFCTL_BUSY;
    slab->freeCount--;
    return idx;
}

void slabFreeListPush(Slab *slab, uint16_t index) {
    uint16_t *bufctl = slabBufctl(slab);
    bufctl[index] = slab->freeHead;
    slab->freeHead = index;
    slab->freeCount++;
}
