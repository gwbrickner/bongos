/* The slab allocator and kmalloc (D-092..D-096, ROADMAP M2.4): ties slab-core.c's pure layout/
 * bufctl math and slab-debug.c's pure redzone/poison checks together with the pmm/vmm, one
 * magazine per cache (D-094), and misuse detection (D-096) reported through panicBug()/
 * TRAP_CATCH_KERNEL_BUG (D-082's existing mechanism). See kernel/include/kmalloc.h. */
#include "slab-internal.h"

#include "bootinfo.h"
#include "cpu-local.h"
#include "klog.h"
#include "panic.h"
#include "pmm.h"
#include "spinlock.h"
#include "vmm.h"

#include <arch/cpu.h>
#include <stddef.h>
#include <stdint.h>

extern void *memset(void *dst, int value, size_t n);

#define SLAB_MAX_CACHES 64

static SlabCache slabCaches[SLAB_MAX_CACHES];
static bool slabCacheInUse[SLAB_MAX_CACHES];

/* Per-CPU magazines (D-094, D-199): one SlabMagazine per cache slot per CPU, reached through
 * cpuLocal()->slab. Still protected by the one global slabLock (so another CPU's magazines can be
 * flushed or counted under it); `slabCpus[]` registers every attached blob under slabLock. */
typedef struct SlabCpu {
    SlabMagazine mags[SLAB_MAX_CACHES];
} SlabCpu;
static SlabCpu slabBspCpu;
static SlabCpu *slabCpus[CPU_MAX] = {&slabBspCpu};
static uint32_t slabCpuCount = 1;

static SlabMagazine *slabLocalMag(const SlabCache *cache) {
    return &((SlabCpu *)cpuLocal()->slab)->mags[cache->slot];
}
static SlabCache *slabKmallocCaches[SLAB_KMALLOC_CLASS_COUNT];

static SlabBugKind slabLastBugKind = SLAB_BUG_NONE;
static const void *slabLastBugObj = NULL;

#ifdef KERNEL_DEBUG
static const bool slabDebugBuild = true;
#else
static const bool slabDebugBuild = false;
#endif

/* --- lock: one coarse irqsave Spinlock for the whole subsystem (registry + every cache + every
 * magazine) -- D-094's deliberate simplification of the architect's proposed three-tier lock
 * split, now a real lock (D-188). The growth and release paths drop it before calling
 * pmmAllocPages()/pmmFreePages() and retake it afterwards (so no slab -> pmm order is ever
 * recorded); the validator allows that repeated and out-of-order acquisition. */
static Spinlock slabLockObj = SPINLOCK_INIT("slab");
static uint64_t slabLock(void) {
    return spinLockIrqSave(&slabLockObj);
}
static void slabUnlock(uint64_t flags) {
    spinUnlockIrqRestore(&slabLockObj, flags);
}

static _Noreturn void slabBug(SlabBugKind kind, const void *obj) {
    slabLastBugKind = kind;
    slabLastBugObj = obj;
    static const char *const names[] = {
        [SLAB_BUG_NONE] = "none",
        [SLAB_BUG_BAD_POINTER] = "bad pointer",
        [SLAB_BUG_VMALLOC_POINTER] = "vmalloc pointer passed to slab free",
        [SLAB_BUG_NOT_SLAB] = "not a slab page",
        [SLAB_BUG_MISALIGNED] = "misaligned/interior pointer",
        [SLAB_BUG_WRONG_CACHE] = "wrong cache",
        [SLAB_BUG_DOUBLE_FREE] = "double free",
        [SLAB_BUG_CORRUPT] = "corrupt slab metadata",
        [SLAB_BUG_BAD_SIZE] = "bad size",
        [SLAB_BUG_BAD_FLAGS] = "bad flags",
        [SLAB_BUG_CACHE_BUSY] = "cache busy",
        [SLAB_BUG_REDZONE] = "redzone corrupted",
        [SLAB_BUG_POISON] = "poison mismatch (write after free)",
    };
    panicBug("slab: %s obj=%p", names[kind], obj);
}

SlabBugKind slabTakeLastBug(const void **outObj) {
    uint64_t flags = slabLock();
    SlabBugKind kind = slabLastBugKind;
    if (outObj != NULL) {
        *outObj = slabLastBugObj;
    }
    slabLastBugKind = SLAB_BUG_NONE;
    slabLastBugObj = NULL;
    slabUnlock(flags);
    return kind;
}

/* --- pointer resolution: HHDM address -> (Slab*, index) --- */

typedef struct {
    Slab *slab;
    uint32_t index;
} SlabResolved;

static bool slabResolvePointer(const void *ptr, SlabResolved *out, SlabBugKind *bugOut) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    if (addr >= VM_KVA_BASE && addr < VM_KVA_END) {
        *bugOut = SLAB_BUG_VMALLOC_POINTER;
        return false;
    }
    uint64_t hhdmBase = pmmHhdmBase();
    if (addr < hhdmBase || addr - hhdmBase >= BOOTINFO_HHDM_SIZE) {
        *bugOut = SLAB_BUG_BAD_POINTER;
        return false;
    }
    uint64_t phys = addr - hhdmBase;
    Page *page = pmmPhysToPage(phys);
    if (page == NULL || !(page->flags & PAGE_F_SLAB)) {
        *bugOut = SLAB_BUG_NOT_SLAB;
        return false;
    }
    Slab *slab = (Slab *)(uintptr_t)page->privateWord;
    if (slab == NULL || slab->magic != SLAB_MAGIC) {
        *bugOut = SLAB_BUG_CORRUPT;
        return false;
    }
    if (addr < slab->objBase) {
        *bugOut = SLAB_BUG_MISALIGNED;
        return false;
    }
    int32_t idx = slabIndexForOffset(&slab->cache->layout, addr - slab->objBase);
    if (idx < 0) {
        *bugOut = SLAB_BUG_MISALIGNED;
        return false;
    }
    out->slab = slab;
    out->index = (uint32_t)idx;
    return true;
}

/* --- growth/release: NEVER called with slabLock held (see the lock comment above) -- both call
 * into the pmm (which can itself panic via pmmBug(), e.g. on a real allocator bug) and run
 * caller-supplied ctor/dtor callbacks, neither of which may run under slabLock: a panicBug() while
 * slabLock is held would leave IRQs disabled forever after a ktest's caught longjmp resumes,
 * exactly the hazard pmmBug()'s own contract comment warns about (kernel/mm/pmm.c). Callers drop
 * slabLock before calling these and reacquire it after (slabRefill/slabReclaimEmpty below). */

static Slab *slabGrow(SlabCache *cache) {
    Page *head;
    Status st = pmmAllocPages(cache->layout.order, 0, &head);
    if (st != STATUS_OK) {
        return NULL;
    }
    uint64_t phys = pmmPageToPhys(head);
    Slab *slab = (Slab *)(uintptr_t)(pmmHhdmBase() + phys);
    slab->magic = SLAB_MAGIC;
    slab->cache = cache;
    slab->objBase = (uint64_t)(uintptr_t)slab + cache->layout.objOffset;
    slab->objCount = (uint16_t)cache->layout.objsPerSlab;
    slab->order = (uint8_t)cache->layout.order;
    slab->list = SLAB_LIST_NONE;
    slabCarve(slab);

    for (uint64_t p = 0; p < ((uint64_t)1 << slab->order); p++) {
        Page *pg = pmmPhysToPage(phys + (p << 12));
        pg->flags = (uint16_t)(pg->flags | PAGE_F_SLAB);
        pg->privateWord = (uint64_t)(uintptr_t)slab;
    }

#ifdef KERNEL_DEBUG
    /* Before the ctor, not after: a ctor cache never poisons its payload (slabDebugCarveFill()
     * skips it when cache->ctor != NULL), so the only thing this fill loop actually touches for
     * such a cache is the redzones -- running it first means a ctor that overruns its own object
     * corrupts a redzone the very same way a caller overrun would, and gets caught the same way
     * (slabAlloc()'s redzone check on first use), instead of the ctor's own out-of-bounds write
     * silently overwriting whatever this fill would have put there. */
    for (uint32_t i = 0; i < slab->objCount; i++) {
        slabDebugCarveFill(cache, slabSlot(slab, i));
    }
#endif
    if (cache->ctor != NULL) {
        for (uint32_t i = 0; i < slab->objCount; i++) {
            cache->ctor(slabSlot(slab, i));
        }
    }

    return slab;
}

static void slabReleaseToPmm(SlabCache *cache, Slab *slab) {
    if (cache->dtor != NULL) {
        for (uint32_t i = 0; i < slab->objCount; i++) {
            cache->dtor(slabSlot(slab, i));
        }
    }
    uint64_t basePhys = (uint64_t)(uintptr_t)slab - pmmHhdmBase();
    slab->magic = 0;
    for (uint64_t p = 0; p < ((uint64_t)1 << slab->order); p++) {
        Page *pg = pmmPhysToPage(basePhys + (p << 12));
        pg->flags = (uint16_t)(pg->flags & ~PAGE_F_OWNER_MASK);
        pg->privateWord = 0;
    }
    pmmFreePages(pmmPhysToPage(basePhys), slab->order);
}

/* Moves `slab` to the list its current freeCount implies, if it isn't there already, keeping
 * cache->emptySlabCount in sync. Called with slabLock held. */
static void slabRehomeLocked(SlabCache *cache, Slab *slab) {
    SlabListKind want = slab->freeCount == 0                ? SLAB_LIST_FULL
                        : slab->freeCount == slab->objCount ? SLAB_LIST_EMPTY
                                                            : SLAB_LIST_PARTIAL;
    if (slab->list == want) {
        return;
    }
    if (slab->list == SLAB_LIST_EMPTY) {
        cache->emptySlabCount--;
    }
    listRemove(&slab->link);
    ListNode *destHead = want == SLAB_LIST_FULL    ? &cache->full
                         : want == SLAB_LIST_EMPTY ? &cache->empty
                                                   : &cache->partial;
    listPushHead(destHead, &slab->link);
    if (want == SLAB_LIST_EMPTY) {
        cache->emptySlabCount++;
    }
    slab->list = want;
}

/* Reclaims empty slabs beyond `keep`. Called with slabLock held; `*irqFlags` is the caller's own
 * saved flags, updated in place across the lock drop/retake this needs around each
 * slabReleaseToPmm() call (see the growth/release comment above) -- the caller must keep using
 * `*irqFlags` (not a stale copy) for its own eventual slabUnlock(). */
static void slabReclaimEmptyLocked(SlabCache *cache, uint64_t keep, uint64_t *irqFlags) {
    while (cache->emptySlabCount > keep) {
        ListNode *node = cache->empty.next;
        Slab *slab = LIST_CONTAINER(node, Slab, link);
        listRemove(node);
        cache->emptySlabCount--;
        cache->slabCount--;
        slabUnlock(*irqFlags);
        slabReleaseToPmm(cache, slab);
        *irqFlags = slabLock();
    }
}

/* Pushes one object back onto its own owning slab's free list (which may differ from the slab the
 * object that triggered this flush belongs to -- a magazine's rounds can come from any slab of the
 * cache) and rehomes that slab. Called with slabLock held; `irqFlags` is the caller's own saved
 * flags from that slabLock() call, so a corrupt pointer (unreachable in practice -- these are our
 * own previously-validated magazine entries) can unlock properly before calling slabBug(), rather
 * than nest another archIrqSave() here and hand slabUnlock() a flags value that looks
 * already-disabled, which would leave IRQs off forever after a caught panic's longjmp. */
static void slabPushObjectLocked(SlabCache *cache, void *ptr, uint64_t irqFlags) {
    SlabResolved r;
    SlabBugKind bug;
    if (!slabResolvePointer(ptr, &r, &bug) || r.slab->cache != cache) {
        slabUnlock(irqFlags);
        slabBug(SLAB_BUG_CORRUPT, ptr);
    }
    if (slabBufctl(r.slab)[r.index] != SLAB_BUFCTL_MAG) {
        slabUnlock(irqFlags);
        slabBug(SLAB_BUG_CORRUPT, ptr);
    }
    slabFreeListPush(r.slab, (uint16_t)r.index);
    slabRehomeLocked(cache, r.slab);
}

/* Called with slabLock held; `*irqFlags` is updated in place across the lock drop/retake a grow
 * needs (see the growth/release comment above) -- the caller must keep using `*irqFlags` (not a
 * stale copy) for its own eventual slabUnlock(). */
static void slabRefillLocked(SlabCache *cache, SlabMagazine *mag, uint64_t *irqFlags) {
    uint32_t need = mag->batch;
    /* `mag->count < mag->capacity` is re-checked on every pass: the lock is dropped to grow, and a
     * handler on this CPU may free into this very magazine meanwhile (D-200). */
    while (need > 0 && mag->count < mag->capacity) {
        Slab *slab;
        if (!listEmpty(&cache->partial)) {
            slab = LIST_CONTAINER(cache->partial.next, Slab, link);
        } else if (!listEmpty(&cache->empty)) {
            slab = LIST_CONTAINER(cache->empty.next, Slab, link);
        } else {
            slabUnlock(*irqFlags);
            Slab *newSlab = slabGrow(cache);
            *irqFlags = slabLock();
            if (newSlab == NULL) {
                break;
            }
            listPushHead(&cache->empty, &newSlab->link);
            newSlab->list = SLAB_LIST_EMPTY;
            cache->slabCount++;
            cache->emptySlabCount++;
            slab = newSlab;
        }
        while (need > 0 && slab->freeCount > 0 && mag->count < mag->capacity) {
            uint16_t idx = slabFreeListPop(slab);
            slabBufctl(slab)[idx] = SLAB_BUFCTL_MAG;
            mag->rounds[mag->count++] = slabSlot(slab, idx);
            need--;
        }
        slabRehomeLocked(cache, slab);
    }
}

/* --- public API --- */

/* Resets one CPU's magazine for `cache`: empty, with the cache's capacity and batch (D-094). */
static void slabMagInit(SlabMagazine *mag, const SlabCache *cache) {
    uint32_t cap = cache->layout.stride == 0 ? 32 : 16384u / cache->layout.stride;
    if (cap < 4) {
        cap = 4;
    }
    if (cap > SLAB_MAG_MAX_ROUNDS) {
        cap = SLAB_MAG_MAX_ROUNDS;
    }
    *mag = (SlabMagazine){0};
    mag->capacity = (uint16_t)cap;
    mag->batch = (uint16_t)(cap / 2);
}

static Status slabCacheCreateInternal(const char *name, size_t objSize, size_t align,
                                      SlabObjFn ctor, SlabObjFn dtor, uint32_t extraFlags,
                                      SlabCache **outCache) {
    if (name == NULL || outCache == NULL || objSize == 0 || objSize > KMALLOC_MAX_SIZE) {
        return STATUS_ERR_INVALID;
    }
    if (align == 0) {
        align = KMALLOC_MIN_ALIGN;
    }
    if (align < 8 || align > 4096 || (align & (align - 1)) != 0) {
        return STATUS_ERR_INVALID;
    }

    uint64_t flags = slabLock();
    int32_t slot = -1;
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++) {
        if (!slabCacheInUse[i]) {
            slot = (int32_t)i;
            slabCacheInUse[i] = true;
            break;
        }
    }
    slabUnlock(flags);
    if (slot < 0) {
        return STATUS_ERR_NO_MEMORY;
    }

    SlabCache *cache = &slabCaches[slot];
    *cache = (SlabCache){0};
    cache->magic = SLAB_CACHE_MAGIC;
    cache->slot = (uint32_t)slot;
    uint32_t i = 0;
    for (; i < SLAB_NAME_MAX - 1 && name[i] != '\0'; i++) {
        cache->name[i] = name[i];
    }
    cache->name[i] = '\0';
    cache->objSize = (uint32_t)objSize;
    cache->align = (uint32_t)align;
    cache->ctor = ctor;
    cache->dtor = dtor;
    cache->flags = extraFlags;
    slabComputeLayout(cache->objSize, cache->align, slabDebugBuild, &cache->layout);
    listInit(&cache->partial);
    listInit(&cache->full);
    listInit(&cache->empty);

    flags = slabLock();
    for (uint32_t c = 0; c < slabCpuCount; c++) {
        slabMagInit(&slabCpus[c]->mags[slot], cache);
    }
    slabUnlock(flags);

    *outCache = cache;
    return STATUS_OK;
}

Status slabCacheCreate(const char *name, size_t objSize, size_t align, SlabObjFn ctor,
                       SlabObjFn dtor, SlabCache **outCache) {
    return slabCacheCreateInternal(name, objSize, align, ctor, dtor, 0, outCache);
}

/* Flushes the cache's magazine on EVERY attached CPU back to its slabs (D-199). */
static void slabCacheFlushMagazineLocked(SlabCache *cache, uint64_t irqFlags) {
    for (uint32_t c = 0; c < slabCpuCount; c++) {
        SlabMagazine *mag = &slabCpus[c]->mags[cache->slot];
        for (uint32_t i = 0; i < mag->count; i++) {
            slabPushObjectLocked(cache, mag->rounds[i], irqFlags);
            mag->rounds[i] = NULL;
        }
        mag->count = 0;
    }
}

void slabCacheShrink(SlabCache *cache) {
    if (cache == NULL) {
        return;
    }
    uint64_t flags = slabLock();
    slabCacheFlushMagazineLocked(cache, flags);
    slabReclaimEmptyLocked(cache, 0, &flags);
    slabUnlock(flags);
}

void slabShrinkAll(void) {
    uint64_t flags = slabLock();
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++) {
        if (!slabCacheInUse[i]) {
            continue;
        }
        SlabCache *cache = &slabCaches[i];
        slabCacheFlushMagazineLocked(cache, flags);
        slabReclaimEmptyLocked(cache, 0, &flags);
    }
    slabUnlock(flags);
}

/* True if `cache` is genuinely a live, in-use slot of `slabCaches[]` -- guards against a wild
 * pointer (out of bounds, or not exactly on an element boundary) before ever dereferencing it, and
 * against a cache slot that's already been destroyed and could since have been reused for a
 * different cache (SLAB_CACHE_MAGIC catches that: slabCacheCreateInternal() sets it, this
 * function's caller clears it before the slot is released back to the registry). Called with
 * slabLock held. */
static bool slabCacheIsLiveLocked(const SlabCache *cache) {
    uintptr_t off = (uintptr_t)cache - (uintptr_t)slabCaches;
    if (off % sizeof(SlabCache) != 0) {
        return false;
    }
    uint64_t slot = off / sizeof(SlabCache);
    if (slot >= SLAB_MAX_CACHES || !slabCacheInUse[slot]) {
        return false;
    }
    return cache->magic == SLAB_CACHE_MAGIC;
}

void slabCacheDestroy(SlabCache *cache) {
    if (cache == NULL) {
        return;
    }
    uint64_t flags = slabLock();
    if (!slabCacheIsLiveLocked(cache)) {
        slabUnlock(flags);
        slabBug(SLAB_BUG_CORRUPT, NULL);
    }
    if (cache->flags & SLAB_CACHE_PERMANENT) {
        slabUnlock(flags);
        slabBug(SLAB_BUG_CACHE_BUSY, NULL);
    }
    slabCacheFlushMagazineLocked(cache, flags);
    slabReclaimEmptyLocked(cache, 0, &flags);
    bool busy = !listEmpty(&cache->partial) || !listEmpty(&cache->full);
    if (busy) {
        slabUnlock(flags);
        slabBug(SLAB_BUG_CACHE_BUSY, NULL);
    }
    cache->magic = 0;
    uint32_t slot = (uint32_t)(cache - slabCaches);
    slabCacheInUse[slot] = false;
    slabUnlock(flags);
}

void *slabAlloc(SlabCache *cache, KmallocFlags flags) {
    if (cache == NULL) {
        slabBug(SLAB_BUG_CORRUPT, NULL);
    }
    if ((flags & ~(KmallocFlags)KMALLOC_FLAGS_VALID) != 0) {
        slabBug(SLAB_BUG_BAD_FLAGS, NULL);
    }
    if ((flags & KMALLOC_ZERO) != 0 && cache->ctor != NULL) {
        slabBug(SLAB_BUG_BAD_FLAGS, NULL);
    }

    uint64_t irqFlags = slabLock();
    if (!slabCacheIsLiveLocked(cache)) {
        slabUnlock(irqFlags);
        slabBug(SLAB_BUG_CORRUPT, NULL);
    }
    SlabMagazine *mag = slabLocalMag(cache);
    if (mag->count == 0) {
        slabRefillLocked(cache, mag, &irqFlags);
    }
    void *obj = NULL;
    if (mag->count > 0) {
        obj = mag->rounds[--mag->count];
        mag->allocated++;
        SlabResolved r;
        SlabBugKind bug;
        if (!slabResolvePointer(obj, &r, &bug) || slabBufctl(r.slab)[r.index] != SLAB_BUFCTL_MAG) {
            slabUnlock(irqFlags);
            slabBug(SLAB_BUG_CORRUPT, obj);
        }
        slabBufctl(r.slab)[r.index] = SLAB_BUFCTL_BUSY;
    }
    slabUnlock(irqFlags);

    if (obj == NULL) {
        return NULL;
    }

#ifdef KERNEL_DEBUG
    if (!slabDebugCheckPoison(cache, obj)) {
        slabBug(SLAB_BUG_POISON, obj);
    }
    if (!slabDebugCheckRedzones(cache, obj)) {
        slabBug(SLAB_BUG_REDZONE, obj);
    }
#endif
    if (flags & KMALLOC_ZERO) {
        memset(obj, 0, cache->objSize);
    }
    return obj;
}

/* Free is three steps (D-199): claim (under the lock: the object must be BUSY, and becomes FREEING,
 * so two CPUs racing to free the same pointer cannot both pass), the unlocked KERNEL_DEBUG redzone
 * check and poison fill, then push onto this CPU's magazine (MAG). The poison fill must finish
 * before the push, or a handler on this CPU could allocate the object half-poisoned. */
static void slabFreeCommon(SlabCache *cache, const SlabResolved *r, void *ptr) {
    uint16_t *bufctl = slabBufctl(r->slab);

    uint64_t irqFlags = slabLock();
    bool dup = bufctl[r->index] != SLAB_BUFCTL_BUSY;
    if (!dup) {
        bufctl[r->index] = SLAB_BUFCTL_FREEING;
    }
    slabUnlock(irqFlags);
    if (dup) {
        slabBug(SLAB_BUG_DOUBLE_FREE, ptr);
    }

#ifdef KERNEL_DEBUG
    if (!slabDebugCheckRedzones(cache, ptr)) {
        irqFlags = slabLock();
        bufctl[r->index] = SLAB_BUFCTL_BUSY; /* the object stays live: nothing was freed */
        slabUnlock(irqFlags);
        slabBug(SLAB_BUG_REDZONE, ptr);
    }
    slabDebugPoisonFree(cache, ptr);
#endif

    irqFlags = slabLock();
    SlabMagazine *mag = slabLocalMag(cache);
    if (mag->count >= mag->capacity) {
        uint32_t flushN = mag->batch;
        for (uint32_t i = 0; i < flushN; i++) {
            slabPushObjectLocked(cache, mag->rounds[i], irqFlags);
            /* Defense in depth against a caught panicBug() partway through this loop (only
             * reachable via corruption -- see slabPushObjectLocked()'s own comment): every round
             * already pushed back onto its slab's own free list is cleared here so it can never
             * also be handed out a second time straight out of this (soon to be discarded)
             * portion of the magazine array. */
            mag->rounds[i] = NULL;
        }
        for (uint32_t i = flushN; i < mag->count; i++) {
            mag->rounds[i - flushN] = mag->rounds[i];
        }
        mag->count -= flushN;
        slabReclaimEmptyLocked(cache, SLAB_EMPTY_KEEP, &irqFlags);
        /* The reclaim dropped the lock: a handler may have used this magazine meanwhile. */
        mag = slabLocalMag(cache);
        if (mag->count >= mag->capacity) {
            slabUnlock(irqFlags);
            slabBug(SLAB_BUG_CORRUPT, ptr);
        }
    }
    bufctl[r->index] = SLAB_BUFCTL_MAG;
    mag->rounds[mag->count++] = ptr;
    mag->allocated--;
    slabUnlock(irqFlags);
}

void slabFree(SlabCache *cache, void *obj) {
    if (obj == NULL) {
        return;
    }
    SlabResolved r;
    SlabBugKind bug;
    if (!slabResolvePointer(obj, &r, &bug)) {
        slabBug(bug, obj);
    }
    if (r.slab->cache != cache) {
        slabBug(SLAB_BUG_WRONG_CACHE, obj);
    }
    slabFreeCommon(cache, &r, obj);
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    SlabResolved r;
    SlabBugKind bug;
    if (!slabResolvePointer(ptr, &r, &bug)) {
        slabBug(bug, ptr);
    }
    if (!(r.slab->cache->flags & SLAB_CACHE_KMALLOC)) {
        slabBug(SLAB_BUG_WRONG_CACHE, ptr);
    }
    slabFreeCommon(r.slab->cache, &r, ptr);
}

void *kmalloc(size_t size, KmallocFlags flags) {
    uint32_t classIdx = slabClassIndexForSize(size);
    if (classIdx == UINT32_MAX) {
        slabBug(SLAB_BUG_BAD_SIZE, NULL);
    }
    return slabAlloc(slabKmallocCaches[classIdx], flags);
}

SlabCache *kmallocCacheForSize(size_t size) {
    uint32_t classIdx = slabClassIndexForSize(size);
    return classIdx == UINT32_MAX ? NULL : slabKmallocCaches[classIdx];
}

static uint64_t slabSumFreeLocked(const ListNode *head) {
    uint64_t sum = 0;
    for (const ListNode *n = head->next; n != head; n = n->next) {
        sum += LIST_CONTAINER(n, Slab, link)->freeCount;
    }
    return sum;
}

void slabCacheGetStats(const SlabCache *cache, SlabCacheStats *out) {
    uint64_t flags = slabLock();
    if (cache == NULL || !slabCacheIsLiveLocked(cache)) {
        slabUnlock(flags);
        slabBug(SLAB_BUG_CORRUPT, NULL);
    }
    *out = (SlabCacheStats){0};
    out->name = cache->name;
    out->objSize = cache->objSize;
    out->stride = cache->layout.stride;
    out->align = cache->align;
    out->order = cache->layout.order;
    out->objsPerSlab = cache->layout.objsPerSlab;
    out->slabs = cache->slabCount;
    out->emptySlabs = cache->emptySlabCount;

    uint64_t freeObjs = slabSumFreeLocked(&cache->partial) + slabSumFreeLocked(&cache->full) +
                        slabSumFreeLocked(&cache->empty);
    out->objsFree = freeObjs;
    uint64_t cached = 0;
    for (uint32_t c = 0; c < slabCpuCount; c++) {
        cached += slabCpus[c]->mags[cache->slot].count;
    }
    out->objsCached = cached;
    out->objsAllocated = cache->slabCount * cache->layout.objsPerSlab - freeObjs - cached;
    slabUnlock(flags);
}

size_t slabCpuBlobSize(void) {
    return sizeof(SlabCpu);
}

Status slabCpuAttach(CpuLocal *cl, void *storage) {
    if (cl == NULL || storage == NULL || cl->slab != NULL) {
        return STATUS_ERR_INVALID;
    }
    SlabCpu *c = storage;
    uint64_t flags = slabLock();
    if (slabCpuCount >= CPU_MAX) {
        slabUnlock(flags);
        return STATUS_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++) {
        if (slabCacheInUse[i]) {
            slabMagInit(&c->mags[i], &slabCaches[i]);
        } else {
            c->mags[i] = (SlabMagazine){0};
        }
    }
    slabCpus[slabCpuCount++] = c;
    cl->slab = c;
    slabUnlock(flags);
    return STATUS_OK;
}

void slabInit(void) {
    cpuLocal()->slab = &slabBspCpu;
    for (uint32_t i = 0; i < SLAB_KMALLOC_CLASS_COUNT; i++) {
        char name[SLAB_NAME_MAX];
        uint32_t sz = slabKmallocClassSizes[i];
        /* No ksnprintf here: this runs before kmalloc exists for anything else that might want
         * it, and the format is trivial enough to build by hand. */
        const char prefix[] = "kmalloc-";
        uint32_t p = 0;
        for (; prefix[p] != '\0'; p++) {
            name[p] = prefix[p];
        }
        char digits[8];
        uint32_t d = 0;
        do {
            digits[d++] = (char)('0' + (sz % 10));
            sz /= 10;
        } while (sz > 0);
        while (d > 0) {
            name[p++] = digits[--d];
        }
        name[p] = '\0';

        SlabCache *cache;
        Status st =
            slabCacheCreateInternal(name, slabKmallocClassSizes[i], KMALLOC_MIN_ALIGN, NULL, NULL,
                                    SLAB_CACHE_PERMANENT | SLAB_CACHE_KMALLOC, &cache);
        if (st != STATUS_OK) {
            panic("slab: failed to create %s (status %d)", name, (int)st);
        }
        slabKmallocCaches[i] = cache;
    }
    klogWrite(KLOG_INFO, "slab", "kmalloc: %u size classes, 16..%u bytes",
              (unsigned)SLAB_KMALLOC_CLASS_COUNT, (unsigned)KMALLOC_MAX_SIZE);
}
