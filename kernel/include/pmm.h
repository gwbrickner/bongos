/* The physical memory manager (ARCHITECTURE §6.2, D-079..D-082, ROADMAP M2.2): a bump allocator
 * used only during pmmInit(), backing a buddy allocator (orders 0-10, DMA32/NORMAL zones) with a
 * BSP-only per-CPU page cache in front of it for order-0 allocations. `kernel/mm/pmm-internal.h`
 * has the zone/cache structs and the bump/buddy internals; this header is the public surface every
 * other subsystem uses. */
#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include "bootinfo.h"
#include "page.h"
#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

#define PMM_MAX_ORDER   10
#define PMM_ORDER_COUNT (PMM_MAX_ORDER + 1)

/* The Page array (VM_PAGE_ARRAY_BASE..VM_PAGE_ARRAY_END, page.h) must be able to hold one Page
 * per frame across the entire 64 TiB HHDM window -- confirms the fixed VA region ARCHITECTURE
 * §6.1 sets aside for it is actually big enough (1 TiB needed, 16 TiB available). */
_Static_assert((BOOTINFO_HHDM_SIZE >> 12) * sizeof(Page) <= VM_PAGE_ARRAY_END - VM_PAGE_ARRAY_BASE,
               "the Page-array VA region is too small for the full HHDM window");

typedef enum { PMM_ZONE_DMA32 = 0, PMM_ZONE_NORMAL = 1, PMM_ZONE_COUNT = 2 } PmmZoneId;

typedef uint32_t PmmFlags;
#define PMM_FLAG_DMA32  (1u << 0) /* the block must lie entirely below 4 GiB */
#define PMM_FLAG_ZERO   (1u << 1) /* zero-fill the block before returning it */
#define PMM_FLAGS_VALID (PMM_FLAG_DMA32 | PMM_FLAG_ZERO)

typedef struct {
    uint64_t usablePages;  /* BootInfo USABLE total */
    uint64_t managedPages; /* handed to the buddy allocator via pmmAddFreeRange */
    uint64_t freePages;    /* managedPages currently free: buddy free lists + every cache */
    uint64_t cachedPages;  /* subset of freePages sitting in the (BSP) per-CPU cache */
    uint64_t allocatedPages;
    uint64_t earlyPages;       /* consumed by the bump allocator (Page array + its page tables) */
    uint64_t pageArrayPages;   /* subset of earlyPages: the Page array itself */
    uint64_t pageTablePages;   /* subset of earlyPages: page tables mapping the Page array */
    uint64_t lowReservedPages; /* USABLE below 1 MiB, withheld from the buddy allocator */
    uint64_t unmappedPages;    /* USABLE at or beyond the 64 TiB HHDM window */
    uint64_t reclaimedPages;   /* LOADER_RECLAIM handed to the buddy allocator (M2.3, D-089) */
    uint64_t zoneManagedPages[PMM_ZONE_COUNT];
    uint64_t zoneFreePages[PMM_ZONE_COUNT];
    uint64_t typePages[BOOT_MEM_FRAMEBUFFER + 1]; /* indexed by BootMemType */
} PmmStats;

typedef enum {
    PMM_BUG_NONE = 0,
    PMM_BUG_INVALID_PAGE,   /* NULL / outside the array / misaligned / !pmmPfnValid */
    PMM_BUG_BAD_ORDER,      /* order > PMM_MAX_ORDER */
    PMM_BUG_MISALIGNED,     /* pfn not a multiple of 2^order */
    PMM_BUG_ORDER_MISMATCH, /* freeing an allocated head with the wrong order */
    PMM_BUG_DOUBLE_FREE,
    PMM_BUG_NOT_HEAD,       /* freeing an interior page of a still-live block */
    PMM_BUG_RESERVED_FRAME, /* freeing a frame the pmm never owned */
    PMM_BUG_CORRUPT_STATE,  /* a Page.state value that shouldn't be reachable */
    PMM_BUG_POISON,         /* KERNEL_DEBUG: an allocated page's poison pattern was overwritten */
    PMM_BUG_OWNED_PAGE      /* M2.4/D-095: freeing a page an owner (slab/vmalloc) never released */
} PmmBugKind;

/* Scans `bi`'s memory map, builds the Page array over it, brings up the DMA32/NORMAL zones and the
 * BSP page cache, and frees every USABLE page (at or above 1 MiB, below the 64 TiB HHDM window) to
 * the buddy allocator. Panics on any failure (not enough memory for the Page array, an
 * unrecoverable map inconsistency, an unmapped HHDM region for a managed type) -- there is no
 * partially-initialized pmm to recover from. Boot-time only, BSP, IF=0, not reentrant; called once
 * from kernelMain after klogInit(). */
void pmmInit(const BootInfo *bi);

/* On success: `*outPage` is the head of a naturally-aligned, contiguous 2^order-frame block (state
 * ALLOCATED, refcount 1) and this returns STATUS_OK. On failure: `*outPage` is NULL and this
 * returns STATUS_ERR_INVALID (order > PMM_MAX_ORDER, an unknown flag, or outPage == NULL) or
 * STATUS_ERR_NO_MEMORY (no eligible zone has a free block of that order). Every order-0 request
 * (DMA32-flagged included -- the per-CPU cache has one free list per zone) goes through the local
 * per-CPU cache first. Locks: pmmLock (M2.2: IRQ-disable only, single CPU; a real lock arrives
 * with SMP, M3.4/M3.5). IRQ-safe: yes. May sleep: no. */
Status pmmAllocPages(uint32_t order, PmmFlags flags, Page **outPage);

/* Returns a block pmmAllocPages handed out, with the same `order` it was allocated at. Cannot
 * fail: any misuse (a bad pointer, wrong order, a double free, freeing an interior or a reserved
 * page) is a kernel bug and panics via panicBug() (kernel/include/panic.h) -- state is validated
 * *before* anything is mutated, so a caught panic (ktest's archTrapCatch) never leaves the pmm
 * half-updated. Locks: pmmLock. IRQ-safe: yes. May sleep: no. */
void pmmFreePages(Page *page, uint32_t order);

/* Hands `[physBase, physBase + length)` to the buddy allocator -- the only way memory enters it.
 * Requires every frame in the range to already have a Page entry (pmmPfnValid) in state RESERVED;
 * returns STATUS_ERR_INVALID otherwise (misaligned bounds, an unbacked or already-managed frame)
 * with nothing changed. pmmInit() uses this for BootInfo's USABLE ranges; later milestones reuse
 * it for LOADER_RECLAIM (M2.3), ACPI_RECLAIM (M3.1), and INITRD (M5.5). Locks: pmmLock. IRQ-safe:
 * yes. May sleep: no. */
Status pmmAddFreeRange(uint64_t physBase, uint64_t length);

/* Drains the calling CPU's local page cache back to the buddy free lists (M2.2: the one BSP
 * cache). Locks: pmmLock. IRQ-safe: yes. May sleep: no. */
void pmmDrainLocalCache(void);

/* True if `pfn` has a Page entry at all (not necessarily free or even RAM -- see page.h). No
 * locks (span table is immutable after pmmInit); pure. */
bool pmmPfnValid(uint64_t pfn);

/* NULL if !pmmPfnValid(phys >> 12); otherwise pageFromPfn(phys >> 12). No locks; pure. */
Page *pmmPhysToPage(uint64_t phys);

/* True if [phys, phys+4096) lies entirely in the portion of an originally-USABLE range the bump
 * allocator actually consumed during pmmInit() -- i.e. genuinely bump-allocator memory. Used by
 * M2.3's kernel page-table builder (kernel/arch/x86_64/paging.c) to confirm a table page it's
 * about to adopt from the loader's own tables is real bump memory, not a KERNEL/INITRD/
 * ACPI_RECLAIM/LOADER_RECLAIM page that merely happens to share PAGE_STATE_RESERVED with it (all
 * of those stay RESERVED until their own later reclaim runs, so the Page state alone can't tell
 * them apart from bump memory). No locks (span/map data is immutable after pmmInit); pure. */
bool pmmPhysIsEarlyAlloc(uint64_t phys);

static inline uint64_t pmmPageToPhys(const Page *page) {
    return pageToPfn(page) << 12;
}
/* The BootInfo.hhdmBase pmmInit() was given. No locks; IRQ-safe; pure. */
uint64_t pmmHhdmBase(void);
static inline void *pmmPageToVirt(const Page *page) {
    return (void *)(uintptr_t)(pmmHhdmBase() + pmmPageToPhys(page));
}

/* Fills `*out` with a consistent snapshot of the pmm's stats (see PmmStats above). Locks: pmmLock.
 * IRQ-safe: yes. May sleep: no. */
void pmmGetStats(PmmStats *out);

/* Prints the ROADMAP M2.2 "/proc/meminfo-style totals" to klog (tag "meminfo"), ending with a
 * self-check line that must read "OK" (usablePages == managedPages + earlyPages +
 * lowReservedPages + unmappedPages). No locks beyond pmmGetStats's; boot-time only. */
void pmmPrintMeminfo(void);

/* Hands every LOADER_RECLAIM range the pmm recorded during pmmInit() (its own `PmmMap.
 * loaderReclaim[]`, ROADMAP M2.3 step 6, D-089) to the buddy allocator via pmmAddFreeRange() --
 * clipped to >= 1 MiB (D-080's low-memory withholding still applies) and with `keepPagePhys`'s
 * page (the BootInfo page, still needed until M2.6) carved out and left RESERVED. Every reclaimed
 * byte is zeroed through the HHDM first (a loader stack or its page-table pool can hold RNG/seed
 * residue or other loader-controlled data). Panics if vmmKernelTablesActive() is false (the loader
 * page tables -- and, on real hardware, its identity-mapped trampoline page -- are part of what
 * gets reclaimed, so this would otherwise free memory the CPU is still using for address
 * translation) or if `keepPagePhys` isn't 4 KiB-aligned. Boot-time only, BSP, IF=0; called once,
 * from kernelMain after vmmInit(). Locks: pmmLock. IRQ-safe: yes. May sleep: no. */
void pmmReclaimLoaderMemory(uint64_t keepPagePhys);

/* The kind of the most recent pmmBug() call, for ktests that catch it via archTrapCatch
 * (TRAP_CATCH_KERNEL_BUG) to assert what actually happened. PMM_BUG_NONE if none has happened yet.
 * No locks; boot-time/ktest only. */
PmmBugKind pmmLastBug(void);

#endif
