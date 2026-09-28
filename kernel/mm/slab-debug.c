/* Pure KERNEL_DEBUG redzone/poison core (D-096, ROADMAP M2.4): no klog, no panic, no arch/pmm
 * calls -- host-tested directly by tests/host/kernel_slab_test.c. Unconditionally compiled (only
 * slab.c's *calls* to these are gated behind #ifdef KERNEL_DEBUG), so these are host-testable in
 * both modes without a build flag, same as slabComputeLayout()'s `debug` parameter. See
 * slab-internal.h. */
#include "slab-internal.h"

#include <stddef.h>

static uint8_t *slabDebugLeftBase(const SlabCache *cache, void *slot) {
    return (uint8_t *)slot - cache->layout.rzLeft;
}
static const uint8_t *slabDebugLeftBaseConst(const SlabCache *cache, const void *slot) {
    return (const uint8_t *)slot - cache->layout.rzLeft;
}
static uint32_t slabDebugRightLen(const SlabCache *cache) {
    return cache->layout.stride - cache->layout.rzLeft - cache->objSize;
}

void slabDebugCarveFill(const SlabCache *cache, void *slot) {
    uint8_t *left = slabDebugLeftBase(cache, slot);
    for (uint32_t i = 0; i < cache->layout.rzLeft; i++) {
        left[i] = SLAB_REDZONE_BYTE;
    }
    uint8_t *right = (uint8_t *)slot + cache->objSize;
    uint32_t rzRight = slabDebugRightLen(cache);
    for (uint32_t i = 0; i < rzRight; i++) {
        right[i] = SLAB_REDZONE_BYTE;
    }
    if (cache->ctor == NULL) {
        uint8_t *payload = (uint8_t *)slot;
        for (uint32_t i = 0; i < cache->objSize; i++) {
            payload[i] = SLAB_POISON_BYTE;
        }
    }
}

bool slabDebugCheckRedzones(const SlabCache *cache, const void *slot) {
    const uint8_t *left = slabDebugLeftBaseConst(cache, slot);
    for (uint32_t i = 0; i < cache->layout.rzLeft; i++) {
        if (left[i] != SLAB_REDZONE_BYTE) {
            return false;
        }
    }
    const uint8_t *right = (const uint8_t *)slot + cache->objSize;
    uint32_t rzRight = slabDebugRightLen(cache);
    for (uint32_t i = 0; i < rzRight; i++) {
        if (right[i] != SLAB_REDZONE_BYTE) {
            return false;
        }
    }
    return true;
}

void slabDebugPoisonFree(const SlabCache *cache, void *slot) {
    if (cache->ctor != NULL) {
        return; /* constructed state must survive a free */
    }
    uint8_t *payload = (uint8_t *)slot;
    for (uint32_t i = 0; i < cache->objSize; i++) {
        payload[i] = SLAB_POISON_BYTE;
    }
}

bool slabDebugCheckPoison(const SlabCache *cache, const void *slot) {
    if (cache->ctor != NULL) {
        return true; /* never poisoned in the first place */
    }
    const uint8_t *payload = (const uint8_t *)slot;
    for (uint32_t i = 0; i < cache->objSize; i++) {
        if (payload[i] != SLAB_POISON_BYTE) {
            return false;
        }
    }
    return true;
}
