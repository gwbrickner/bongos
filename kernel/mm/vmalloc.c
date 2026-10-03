/* vmalloc (D-092/D-097, ROADMAP M2.4): eager, page-granular kernel allocations with guard pages,
 * built on M2.3's vmmMapKernel()/vmmKvaAlloc() and M2.4's slab allocator (for the small
 * VmallocArea records themselves). See kernel/include/vmalloc.h. */
#include "vmalloc.h"

#include "kmalloc.h"
#include "panic.h"
#include "pmm.h"
#include "spinlock.h"
#include "vmm.h"

#include <arch/cpu.h>
#include <stddef.h>
#include <stdint.h>

#define VMALLOC_AREA_MAGIC 0x56A1A0CA56A1A0CAULL

typedef struct {
    uint64_t magic;
    uint64_t va;
    uint64_t pages;
} VmallocArea;

static SlabCache *vmallocAreaCache;
static VmallocBugKind vmallocLastBugKind = VMALLOC_BUG_NONE;
static uint64_t vmallocAreaCountValue;
static uint64_t vmallocPageCountValue;

static Spinlock vmallocLockObj = SPINLOCK_INIT("vmalloc");
static uint64_t vmallocLock(void) {
    return spinLockIrqSave(&vmallocLockObj);
}
static void vmallocUnlock(uint64_t flags) {
    spinUnlockIrqRestore(&vmallocLockObj, flags);
}

static _Noreturn void vmallocBug(VmallocBugKind kind) {
    vmallocLastBugKind = kind;
    static const char *const names[] = {
        [VMALLOC_BUG_NONE] = "none",
        [VMALLOC_BUG_BAD_POINTER] = "bad pointer",
        [VMALLOC_BUG_NOT_MAPPED] = "not mapped",
        [VMALLOC_BUG_NOT_VMALLOC] = "not a vmalloc page",
        [VMALLOC_BUG_NOT_HEAD] = "not the area's head address",
        [VMALLOC_BUG_CORRUPT] = "corrupt area metadata",
        [VMALLOC_BUG_BAD_SIZE] = "bad size",
        [VMALLOC_BUG_BAD_FLAGS] = "bad flags",
    };
    panicBug("vmalloc: %s", names[kind]);
}

VmallocBugKind vmallocTakeLastBug(void) {
    VmallocBugKind kind = vmallocLastBugKind;
    vmallocLastBugKind = VMALLOC_BUG_NONE;
    return kind;
}

void vmallocInit(void) {
    Status st =
        slabCacheCreate("vmalloc-area", sizeof(VmallocArea), 8, NULL, NULL, &vmallocAreaCache);
    if (st != STATUS_OK) {
        panic("vmalloc: failed to create the area cache (status %d)", (int)st);
    }
}

#define VMALLOC_RELEASE_CHUNK 64u

/* Unmaps and frees `count` pages starting at `va`, all of which must be mapped vmalloc pages. The
 * unmap runs in chunks of VMALLOC_RELEASE_CHUNK pages: one vmmUnmapKernel() (hence one TLB
 * shootdown round, D-196) per chunk, and a frame goes back to the pmm only after the shootdown that
 * made it unreachable on every CPU. */
static void vmallocReleasePages(uint64_t va, uint64_t count) {
    uint64_t pas[VMALLOC_RELEASE_CHUNK];
    for (uint64_t done = 0; done < count;) {
        uint64_t n = count - done < VMALLOC_RELEASE_CHUNK ? count - done : VMALLOC_RELEASE_CHUNK;
        for (uint64_t k = 0; k < n; k++) {
            VmmFlags outFlags;
            /* Proven mapped by the caller; a failure here means the vmm's own tables disagree,
             * and freeing the frame anyway would let it be reused while still mapped. */
            if (vmmLookupKernel(va + (done + k) * 4096, &pas[k], &outFlags) != STATUS_OK) {
                vmallocBug(VMALLOC_BUG_CORRUPT);
            }
        }
        if (vmmUnmapKernel(va + done * 4096, n * 4096) != STATUS_OK) {
            vmallocBug(VMALLOC_BUG_CORRUPT);
        }
        for (uint64_t k = 0; k < n; k++) {
            Page *page = pmmPhysToPage(pas[k]);
            page->flags = (uint16_t)(page->flags & ~PAGE_F_OWNER_MASK);
            page->privateWord = 0;
            pmmFreePages(page, 0);
        }
        done += n;
    }
}

/* Unmaps/frees every page of `area` already mapped (indices [0, upTo)) and returns its KVA
 * reservation and the area record itself. Used by vmalloc()'s own unwind on partial failure; the
 * order mirrors vfree()'s teardown -- frames go before the KVA range that addressed them. */
static void vmallocUnwind(VmallocArea *area, uint64_t va, uint64_t upTo, uint64_t mapSize) {
    vmallocReleasePages(va, upTo);
    vmmKvaFree(va, mapSize);
    area->magic = 0;
    slabFree(vmallocAreaCache, area);
}

void *vmalloc(size_t size, VmallocFlags flags) {
    if ((flags & ~(VmallocFlags)VMALLOC_FLAGS_VALID) != 0) {
        vmallocBug(VMALLOC_BUG_BAD_FLAGS);
    }
    if (size == 0) {
        vmallocBug(VMALLOC_BUG_BAD_SIZE);
    }
    if (size > VMALLOC_MAX_SIZE) {
        return NULL;
    }
    uint64_t pages = ((uint64_t)size + 4095) / 4096;
    uint64_t mapSize = pages * 4096;

    VmallocArea *area = (VmallocArea *)slabAlloc(vmallocAreaCache, 0);
    if (area == NULL) {
        return NULL;
    }
    area->magic = VMALLOC_AREA_MAGIC;
    area->pages = 0;

    uint64_t va;
    if (vmmKvaAlloc(mapSize, &va) != STATUS_OK) {
        slabFree(vmallocAreaCache, area);
        return NULL;
    }
    area->va = va;

    for (uint64_t i = 0; i < pages; i++) {
        Page *page;
        PmmFlags pmFlags = (flags & VMALLOC_ZERO) ? PMM_FLAG_ZERO : 0;
        if (pmmAllocPages(0, pmFlags, &page) != STATUS_OK) {
            vmallocUnwind(area, va, i, mapSize);
            return NULL;
        }
        page->flags = (uint16_t)(page->flags | PAGE_F_VMALLOC);
        page->privateWord = (uint64_t)(uintptr_t)area;
        uint64_t pa = pmmPageToPhys(page);
        if (vmmMapKernel(va + i * 4096, pa, 4096, VMM_WRITE) != STATUS_OK) {
            page->flags = (uint16_t)(page->flags & ~PAGE_F_OWNER_MASK);
            page->privateWord = 0;
            pmmFreePages(page, 0);
            vmallocUnwind(area, va, i, mapSize);
            return NULL;
        }
        area->pages = i + 1;
    }

    uint64_t lockFlags = vmallocLock();
    vmallocAreaCountValue++;
    vmallocPageCountValue += pages;
    vmallocUnlock(lockFlags);
    return (void *)(uintptr_t)va;
}

void vfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    if ((va % 4096) != 0 || va < VM_KVA_BASE || va >= VM_KVA_END) {
        vmallocBug(VMALLOC_BUG_BAD_POINTER);
    }

    uint64_t pa0;
    VmmFlags outFlags0;
    if (vmmLookupKernel(va, &pa0, &outFlags0) != STATUS_OK) {
        vmallocBug(VMALLOC_BUG_NOT_MAPPED);
    }
    Page *page0 = pmmPhysToPage(pa0);
    if (page0 == NULL || !(page0->flags & PAGE_F_VMALLOC)) {
        vmallocBug(VMALLOC_BUG_NOT_VMALLOC);
    }
    VmallocArea *area = (VmallocArea *)(uintptr_t)page0->privateWord;
    if (area == NULL || area->magic != VMALLOC_AREA_MAGIC) {
        vmallocBug(VMALLOC_BUG_CORRUPT);
    }
    if (area->va != va) {
        vmallocBug(VMALLOC_BUG_NOT_HEAD);
    }
    uint64_t pages = area->pages;
    uint64_t mapSize = pages * 4096;

    /* Validate every page belongs to this exact area before mutating anything. */
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t pa;
        VmmFlags outFlags;
        if (vmmLookupKernel(va + i * 4096, &pa, &outFlags) != STATUS_OK) {
            vmallocBug(VMALLOC_BUG_NOT_MAPPED);
        }
        Page *page = pmmPhysToPage(pa);
        if (page == NULL || !(page->flags & PAGE_F_VMALLOC) ||
            (VmallocArea *)(uintptr_t)page->privateWord != area) {
            vmallocBug(VMALLOC_BUG_CORRUPT);
        }
    }
    /* A corrupted `area->pages` that's too small would otherwise leave the tail of the real
     * mapping un-freed and hand back a VA whose "unmapped guard" is actually still live -- the
     * loop above only proves every page it *does* walk is genuine, not that it walked far enough.
     * The byte right past `mapSize` must be the unmapped guard page vmmKvaAlloc() reserved. */
    uint64_t guardPa;
    VmmFlags guardFlags;
    if (vmmLookupKernel(va + mapSize, &guardPa, &guardFlags) == STATUS_OK) {
        vmallocBug(VMALLOC_BUG_CORRUPT);
    }

    vmallocReleasePages(va, pages);
    vmmKvaFree(va, mapSize);
    area->magic = 0;
    slabFree(vmallocAreaCache, area);

    uint64_t lockFlags = vmallocLock();
    vmallocAreaCountValue--;
    vmallocPageCountValue -= pages;
    vmallocUnlock(lockFlags);
}

void vmallocGetStats(VmallocStats *out) {
    uint64_t lockFlags = vmallocLock();
    out->areas = vmallocAreaCountValue;
    out->pages = vmallocPageCountValue;
    vmallocUnlock(lockFlags);
}
