/* Internal types shared between kernel/mm's slab sources (slab-core.c, slab-debug.c, slab.c) --
 * kernel/include/kmalloc.h is the public surface. slab-core.c/slab-debug.c are pure (no klog/
 * panic/arch/pmm calls) and host-tested directly by tests/host/kernel_slab_test.c, the same split
 * kernel/mm/pmm-internal.h uses for buddy.c. */
#ifndef KERNEL_MM_SLAB_INTERNAL_H
#define KERNEL_MM_SLAB_INTERNAL_H

#include "kmalloc.h"
#include "list.h"

#include <stdbool.h>
#include <stdint.h>

#define SLAB_MAGIC      0x51AB51AB51AB51ABULL
#define SLAB_BUFCTL_END 0xFFFFu /* free, last on the slab's free list */
#define SLAB_BUFCTL_BUSY                                                                           \
    0xFFFEu /* not on the slab's free list (caller-held or magazine-cached;                        \
             * a magazine scan disambiguates the two for double-free                               \
             * detection -- see slab.c's slabFreeCommon()) */
#define SLAB_MAX_OBJECTS    1024u
#define SLAB_MAX_ORDER      3u /* a slab is at most 4096 << 3 = 32 KiB */
#define SLAB_NAME_MAX       24
#define SLAB_MAG_MAX_ROUNDS 32
#define SLAB_EMPTY_KEEP     1u /* fully-empty slabs a cache keeps around before releasing them */

#define SLAB_REDZONE_SIZE 16u
#define SLAB_POISON_BYTE  0x6Bu /* same byte as PMM_POISON (kernel/mm/pmm.c): both mean "freed" */
#define SLAB_REDZONE_BYTE 0xBBu

typedef enum {
    SLAB_LIST_NONE = 0,
    SLAB_LIST_PARTIAL,
    SLAB_LIST_FULL,
    SLAB_LIST_EMPTY
} SlabListKind;

/* The in-slab header, at the slab's base page (its VA is the HHDM alias of the head page pmm
 * handed out). An out-of-band `uint16_t bufctl[objCount]` free list immediately follows this
 * struct in memory (slabBufctl() below) -- never inside the objects themselves, so a cache's
 * constructor-preserved state and KERNEL_DEBUG's write-after-free poison never fight over the
 * same bytes, and a buffer overflow from one object has no freelist pointer to corrupt. */
typedef struct Slab {
    uint64_t magic;
    struct SlabCache *cache;
    ListNode link;    /* on the owning cache's partial/full/empty list */
    uint64_t objBase; /* VA of slot 0 */
    uint16_t objCount;
    uint16_t freeCount;
    uint16_t freeHead; /* index, or SLAB_BUFCTL_END */
    uint8_t list;      /* SlabListKind */
    uint8_t order;
} Slab;

typedef struct {
    uint16_t count, capacity, batch;
    int64_t allocated; /* stats only: net objects handed to callers via this magazine */
    void *rounds[SLAB_MAG_MAX_ROUNDS];
} SlabMagazine;

/* A cache's fixed layout, computed once by slabComputeLayout() at create time -- every slab a
 * cache ever grows uses the same order/objOffset/stride/rzLeft. */
typedef struct {
    uint32_t order; /* slab size is (4096 << order) bytes */
    uint32_t objsPerSlab;
    uint32_t stride;    /* bytes from one slot to the next */
    uint32_t objOffset; /* bytes from the slab base to slot 0 */
    uint32_t rzLeft;    /* left redzone width inside stride; 0 when `debug` was false */
} SlabLayout;

#define SLAB_CACHE_PERMANENT (1u << 0) /* static storage; slabCacheDestroy() refuses it */
#define SLAB_CACHE_KMALLOC   (1u << 1) /* one of the 12 kmalloc-* caches; kfree() requires this */

struct SlabCache {
    char name[SLAB_NAME_MAX];
    uint32_t objSize;
    uint32_t align;
    SlabObjFn ctor, dtor;
    uint32_t flags;
    SlabLayout layout;

    ListNode partial, full, empty; /* list heads of Slab.link */
    uint64_t slabCount, emptySlabCount;

    SlabMagazine bspMag; /* D-094: today's only "per-CPU" magazine */
};

/* --- slab-core.c: pure layout/bufctl math, host-tested --- */

#define SLAB_KMALLOC_CLASS_COUNT 12
extern const uint32_t slabKmallocClassSizes[SLAB_KMALLOC_CLASS_COUNT];

/* Computes the layout for objects of `objSize` bytes, `align`-aligned (caller must have already
 * clamped align >= 8), with redzones sized for KERNEL_DEBUG if `debug` is true. `debug` is a
 * plain bool parameter, not #ifdef-gated, so this is host-testable in both modes without a build
 * flag (D-096). No locks; pure. */
void slabComputeLayout(uint32_t objSize, uint32_t align, bool debug, SlabLayout *out);

/* The kmalloc-* class index for `size` (0 for the 16-byte class, ... , 11 for the 8192-byte
 * class), or UINT32_MAX if size is 0 or over KMALLOC_MAX_SIZE. No locks; pure. */
uint32_t slabClassIndexForSize(size_t size);

/* -1 if `off` (a byte offset from a slab's objBase) isn't exactly at a slot's payload start;
 * otherwise that slot's index. No locks; pure. */
int32_t slabIndexForOffset(const SlabLayout *layout, uint64_t off);

/* Builds the free list 0->1->...->n-1->END over `slab`'s (already objCount-sized) bufctl array.
 * No locks; pure (touches only `slab`'s own bufctl/freeHead/freeCount). */
void slabCarve(Slab *slab);

/* Pops the free-list head (freeCount must be > 0 -- caller's responsibility) and marks it
 * SLAB_BUFCTL_BUSY. Returns its index. No locks; pure. */
uint16_t slabFreeListPop(Slab *slab);

/* Pushes `index` back onto the free list (must currently be SLAB_BUFCTL_BUSY -- caller's
 * responsibility). No locks; pure. */
void slabFreeListPush(Slab *slab, uint16_t index);

static inline uint16_t *slabBufctl(const Slab *slab) {
    return (uint16_t *)(const void *)((const char *)slab + sizeof(Slab));
}
static inline void *slabSlot(const Slab *slab, uint32_t index) {
    return (void *)(uintptr_t)(slab->objBase + (uint64_t)index * slab->cache->layout.stride);
}

/* --- slab-debug.c: KERNEL_DEBUG redzone/poison, pure, host-tested (unconditionally compiled --
 * only slab.c's *calls* to these are gated behind #ifdef KERNEL_DEBUG, so the checks themselves
 * are host-testable in both modes the same way slabComputeLayout()'s `debug` parameter is). --- */

/* Fills the left/right redzones around `slot` (a slab payload pointer) and, for a cache with no
 * constructor, the payload itself with SLAB_POISON_BYTE -- called once when a fresh slab is
 * carved, before any object is ever handed out. No locks; pure. */
void slabDebugCarveFill(const SlabCache *cache, void *slot);

/* True if both redzones around `slot` are still intact. No locks; pure. */
bool slabDebugCheckRedzones(const SlabCache *cache, const void *slot);

/* Poisons `slot`'s payload with SLAB_POISON_BYTE -- a no-op for a cache with a constructor, since
 * constructed state must survive a free. No locks; pure. */
void slabDebugPoisonFree(const SlabCache *cache, void *slot);

/* True if `slot`'s payload is still all SLAB_POISON_BYTE (always true for a ctor cache, which
 * never poisons in the first place). No locks; pure. */
bool slabDebugCheckPoison(const SlabCache *cache, const void *slot);

#endif
