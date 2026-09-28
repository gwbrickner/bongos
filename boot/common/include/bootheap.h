/* stage2's own memory allocator (D-107, ARCHITECTURE §5.6): every loader allocation (file
 * buffers, the kernel image, the boot stack, the BootInfo/page-table pool) comes from here.
 * Bump-only (never freed, never coalesced) and restricted to [BOOT_HEAP_MIN, BOOT_HEAP_MAX) --
 * with paging off, a 32-bit CPU can only address below 4 GiB, and staying above 1 MiB keeps every
 * allocation out of the fixed low-memory scratch layout (docs/specs/bios-boot.md). Pure: takes
 * an already-normalized memory map (memMapNormalize's output, built from E820 via
 * memMapE820TypeToBootMem) rather than doing any I/O itself. */
#ifndef BOOT_COMMON_BOOTHEAP_H
#define BOOT_COMMON_BOOTHEAP_H

#include <stdint.h>

#include "boot-status.h"
#include "bootinfo.h"

#define BOOT_HEAP_MIN         0x100000ULL    /* 1 MiB */
#define BOOT_HEAP_MAX         0x100000000ULL /* 4 GiB */
#define BOOT_HEAP_PAGE_SIZE   4096ULL
#define BOOT_HEAP_MAX_REGIONS 64u

typedef struct {
    uint64_t base; /* bump pointer: bytes below this in the region are already handed out */
    uint64_t end;
} BootHeapRegion;

typedef struct {
    BootHeapRegion regions[BOOT_HEAP_MAX_REGIONS];
    uint32_t count;
} BootHeap;

/* Builds the heap's free-region list from `regions` (a normalized BootMemRegion array, e.g.
 * memMapNormalize's output -- only BOOT_MEM_USABLE entries matter, in the order given), clipping
 * each to [BOOT_HEAP_MIN, BOOT_HEAP_MAX) and to 4 KiB alignment (base up, end down -- never claims
 * a partial page). A region with nothing left after clipping is dropped, not recorded as empty.
 * Returns BOOT_ERR_MEMMAP_CAPACITY if more than BOOT_HEAP_MAX_REGIONS would survive. No locks,
 * boot-time only; pure. */
BootStatus bootHeapInit(BootHeap *heap, const BootMemRegion *regions, uint32_t count);

/* Bumps `pages` * 4 KiB bytes off the lowest region with enough room left (first-fit, bottom-up
 * in region order -- callers pass an already base-sorted map, so this is also address-ascending
 * in practice, though nothing here depends on that). Returns BOOT_ERR_NO_MEMORY if no region has
 * enough room; `*outPhys` is only written on BOOT_OK. No locks, boot-time only; pure. */
BootStatus bootHeapAllocPages(BootHeap *heap, uint32_t pages, uint64_t *outPhys);

#endif
