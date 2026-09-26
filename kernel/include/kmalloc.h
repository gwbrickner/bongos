/* The slab allocator and kmalloc (ARCHITECTURE §6.2 item 5, D-092..D-096, ROADMAP M2.4): named
 * object caches with a fixed size/alignment and optional constructor/destructor, fronted by one
 * magazine per cache (today: the BSP's only, D-094 -- M3.5 makes it real per-CPU state). kmalloc
 * is 12 fixed size classes (16..8192 bytes) built on top of the same machinery. Anything larger
 * goes to vmalloc.h. Never called from kernel/mm/pmm.c or vmm.c (kernel/mm/README.md's
 * no-recursion rule) -- slabInit() runs after both are already up. */
#ifndef KERNEL_KMALLOC_H
#define KERNEL_KMALLOC_H

#include "uapi/status.h"

#include <stddef.h>
#include <stdint.h>

typedef uint32_t KmallocFlags;
#define KMALLOC_ZERO        (1u << 0) /* zero-fill [0, size) before returning; rejected (a kernel
                                       * bug) on a cache with a constructor -- zeroing would
                                       * destroy constructed state the ctor is meant to preserve */
#define KMALLOC_FLAGS_VALID KMALLOC_ZERO
#define KMALLOC_MIN_ALIGN   16u
#define KMALLOC_MAX_SIZE    8192u

typedef struct SlabCache SlabCache; /* opaque; kernel/mm/slab-internal.h has the real definition */
typedef void (*SlabObjFn)(void *obj);

/* D-092: deliberate exception to ARCHITECTURE §4's "kernel functions return a Status" -- an
 * allocator has exactly one runtime failure mode (out of memory), and a sentinel NULL can't be
 * confused with real memory. Every other misuse (bad size, unknown flag, a pointer this subsystem
 * never handed out, a double free) panics via panicBug() (D-082's mechanism), catchable in ktests
 * via archTrapCatch(TRAP_CATCH_KERNEL_BUG) -- see slabTakeLastBug() below. */

/* Brings up the cache-of-caches storage and the 12 `kmalloc-*` size-class caches. Boot-time only,
 * BSP, IF=0; called once from kernelMain right after pmmReclaimLoaderMemory(). Must run before any
 * other call in this header or in vmalloc.h. */
void slabInit(void);

/* Allocates `size` bytes (1..KMALLOC_MAX_SIZE), KMALLOC_MIN_ALIGN-aligned (kmalloc guarantees
 * exactly that -- not natural alignment; a caller needing page alignment uses pmmAllocPages, one
 * needing a specific alignment creates its own cache via slabCacheCreate). Returns NULL only for
 * "no memory"; size 0, size > KMALLOC_MAX_SIZE, or an unknown flag panics via panicBug(). No locks
 * required of the caller; IRQ-safe; never sleeps. */
void *kmalloc(size_t size, KmallocFlags flags);

/* Frees a pointer kmalloc() returned. NULL is a no-op. Any other misuse (a pointer this subsystem
 * never handed out, a pointer from slabAlloc()/vmalloc() instead, a double free, an interior
 * pointer) panics via panicBug(). Same locking/IRQ contract as kmalloc(). */
void kfree(void *ptr);

/* A named object cache with a fixed size/alignment and optional constructor/destructor -- ctor
 * runs once per object when its slab is carved (every object, not just ones actually allocated;
 * dtor the same when the slab is released), so allocated objects come back with their constructed
 * state already set up (Bonwick's "constructed state" pattern). `align` must be a power of two in
 * [8, 4096], or 0 for the default (KMALLOC_MIN_ALIGN). `ctor`/`dtor` (either may be NULL) run with
 * IRQs disabled, must not sleep, and must not allocate from their own cache (or any cache, to keep
 * this simple -- kernel/mm/README.md's no-recursion rule). Returns STATUS_ERR_INVALID for a NULL
 * name/outCache, an objSize of 0 or over KMALLOC_MAX_SIZE, or a bad align; STATUS_ERR_NO_MEMORY if
 * the cache itself (or its first slab) can't be allocated. `name` is copied, truncated to 23
 * bytes. */
Status slabCacheCreate(const char *name, size_t objSize, size_t align, SlabObjFn ctor,
                        SlabObjFn dtor, SlabCache **outCache);

/* Destroys a cache slabCacheCreate() returned. Every object must already be freed back to it
 * (first drains the local magazine and releases every empty slab, so a cache with no *live*
 * caller-held objects always succeeds) -- panics via panicBug() otherwise. Never call this on one
 * of the 12 static kmalloc-* caches (panics). */
void slabCacheDestroy(SlabCache *cache);

/* Allocates/frees one object from `cache`. Same NULL-cache/misuse contract as kmalloc/kfree;
 * `slabFree(cache, obj)` where `obj` didn't come from `cache` panics (SLAB_BUG_WRONG_CACHE). */
void *slabAlloc(SlabCache *cache, KmallocFlags flags);
void slabFree(SlabCache *cache, void *obj);

/* Drains `cache`'s local magazine back to its slabs and releases every slab left fully empty.
 * `slabShrinkAll()` does this for every cache (kmalloc's included) -- ktest/diagnostic use,
 * mirroring pmmDrainLocalCache(). Neither is required before ordinary use. */
void slabCacheShrink(SlabCache *cache);
void slabShrinkAll(void);

/* NULL if `size` is 0 or over KMALLOC_MAX_SIZE; otherwise the static kmalloc-* cache that exact
 * size would come from. Stats/test use -- kmalloc() itself doesn't need this. */
SlabCache *kmallocCacheForSize(size_t size);

typedef struct {
    const char *name;
    uint32_t objSize, stride, align, order, objsPerSlab;
    uint64_t slabs, emptySlabs, objsFree, objsCached, objsAllocated;
} SlabCacheStats;
/* Fills `*out` with a consistent snapshot of `cache`'s stats. Invariant:
 * slabs*objsPerSlab == objsFree+objsCached+objsAllocated. */
void slabCacheGetStats(const SlabCache *cache, SlabCacheStats *out);

typedef enum {
    SLAB_BUG_NONE = 0,
    SLAB_BUG_BAD_POINTER,     /* not an HHDM address, or outside the pmm's managed range */
    SLAB_BUG_VMALLOC_POINTER, /* a KVA (vmalloc) address passed to kfree()/slabFree() */
    SLAB_BUG_NOT_SLAB,        /* an HHDM page without PAGE_F_SLAB (e.g. a plain pmm page) */
    SLAB_BUG_MISALIGNED,      /* an interior pointer: not a slot's payload start */
    SLAB_BUG_WRONG_CACHE,     /* slabFree() on the wrong cache; kfree() of a non-kmalloc object */
    SLAB_BUG_DOUBLE_FREE,
    SLAB_BUG_CORRUPT,         /* bad magic, bad bufctl value, or another impossible state */
    SLAB_BUG_BAD_SIZE,        /* kmalloc(0) or kmalloc(> KMALLOC_MAX_SIZE) */
    SLAB_BUG_BAD_FLAGS,       /* an unknown flag, or KMALLOC_ZERO on a cache with a constructor */
    SLAB_BUG_CACHE_BUSY,      /* slabCacheDestroy() with live objects, or on a static cache */
    SLAB_BUG_REDZONE,         /* KERNEL_DEBUG: a redzone byte was overwritten */
    SLAB_BUG_POISON           /* KERNEL_DEBUG: a freed object's poison was overwritten */
} SlabBugKind;

/* The kind of the most recent slab-subsystem bug, for ktests that catch it via
 * archTrapCatch(TRAP_CATCH_KERNEL_BUG). Clears to SLAB_BUG_NONE on read, so a stale value from an
 * earlier catch can never make a later one look like a pass. `*outObj` (if non-NULL) gets the
 * object pointer involved (NULL if the bug wasn't about a specific pointer). */
SlabBugKind slabTakeLastBug(const void **outObj);

#endif
