/* The `Page` metadata array (ARCHITECTURE §6.1/§6.2, D-079): one 64-byte `Page` per physical frame
 * that the pmm manages, indexed by physical frame number (pfn = phys >> 12) and stored at the
 * fixed VA region 0xFFFFE00000000000-0xFFFFEFFFFFFFFFFF. Backing is sparse -- kernel/mm/pmm-map.c
 * decides which pfn ranges actually get a Page entry -- so `pageFromPfn`/`pageToPfn` are pure
 * address arithmetic and never check that the frame is actually backed; callers that don't already
 * know that (anything crossing an untrusted or unbacked pfn) must call pmmPfnValid() first
 * (kernel/include/pmm.h). */
#ifndef KERNEL_PAGE_H
#define KERNEL_PAGE_H

#include "list.h"

#include <stdint.h>

#define VM_PAGE_ARRAY_BASE 0xFFFFE00000000000ULL /* ARCHITECTURE §6.1 */
#define VM_PAGE_ARRAY_END  0xFFFFF00000000000ULL /* exclusive */

typedef enum {
    PAGE_STATE_RESERVED = 0, /* no pmm owner: firmware/kernel/hole padding/not-yet-reclaimed */
    PAGE_STATE_TAIL = 1,     /* managed, not a block head (interior of a free or allocated block) */
    PAGE_STATE_BUDDY = 2,    /* head of a free block on a zone free list; `order` valid */
    PAGE_STATE_PCP = 3,      /* order-0 page sitting in the per-CPU cache */
    PAGE_STATE_ALLOCATED = 4 /* head of an allocated block; `order` valid */
} PageState;

#define PAGE_F_POISONED (1u << 0) /* KERNEL_DEBUG only: holds PMM_POISON since its last free */
/* M2.4, D-095: ownership of an ALLOCATED page, orthogonal to PageState. At most one of these is
 * ever set; `privateWord` then holds the owning `Slab*`/`VmallocArea*`. The owner clears both the
 * flag and privateWord on every page of its block before pmmFreePages() -- pmmValidateForFree()
 * (kernel/mm/pmm.c) refuses to free a page that still carries one (PMM_BUG_OWNED_PAGE). */
#define PAGE_F_SLAB        (1u << 1) /* kernel/mm/slab.c: privateWord = Slab* */
#define PAGE_F_VMALLOC     (1u << 2) /* kernel/mm/vmalloc.c: privateWord = VmallocArea* */
#define PAGE_F_OWNER_MASK  (PAGE_F_SLAB | PAGE_F_VMALLOC)

/* Deliberately zero-filled == PAGE_STATE_RESERVED, so mapping in a fresh (already-zeroed) page-
 * array page needs no separate init pass (kernel/mm/early.c). `object`/`objectIndex`/`mapcount`
 * are declared now (ARCHITECTURE §6.2) but unused until VmObject exists (M4+); `privateWord` is
 * free for an owner to use (M2.4: PAGE_F_SLAB/PAGE_F_VMALLOC above). */
typedef struct Page {
    uint8_t state;           /* 0  PageState */
    uint8_t order;           /* 1  valid for BUDDY/ALLOCATED heads */
    uint16_t flags;          /* 2  PAGE_F_* */
    uint32_t reserved0;      /* 4  must be 0 */
    int32_t refcount;        /* 8  1 on an allocated head, 0 when free; M4 owns real semantics */
    int32_t mapcount;        /* 12 0 until M4 */
    ListNode lru;            /* 16 free-list / per-CPU-cache link while free; LRU link later */
    struct VmObject *object; /* 32 NULL until M4 (incomplete type: no definition needed yet) */
    uint64_t objectIndex;    /* 40 page index within object; 0 until M4 */
    uint64_t privateWord;    /* 48 owner-private (M2.4: Slab*) */
    uint64_t reserved1;      /* 56 must be 0; spare */
} Page;
_Static_assert(sizeof(Page) == 64, "Page must be exactly 64 bytes (ARCHITECTURE §6.2)");

#ifdef HOSTED
/* Host tests have no real HHDM/page-array VA region to point into, so kernel_pmm_map_test.c and
 * kernel_buddy_test.c back the array with a plain host allocation and set this instead. */
extern uintptr_t hostPageArrayBase;
#define PAGE_ARRAY_VA hostPageArrayBase
#else
#define PAGE_ARRAY_VA VM_PAGE_ARRAY_BASE
#endif

/* Pure address arithmetic (integer math, never pointer subtraction, so a huge pfn can never trip
 * UBSan's pointer-overflow check): does not check that `pfn` is actually backed. No locks; pure.
 */
static inline Page *pageFromPfn(uint64_t pfn) {
    return (Page *)(uintptr_t)(PAGE_ARRAY_VA + pfn * sizeof(Page));
}
static inline uint64_t pageToPfn(const Page *page) {
    return ((uintptr_t)page - PAGE_ARRAY_VA) / sizeof(Page);
}

#endif
