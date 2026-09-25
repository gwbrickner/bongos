/* Portable-facing declarations for pmmInit()'s boot-time page mapper (D-079, ROADMAP M2.2): maps
 * the freshly bump-allocated Page-array pages into the *live* page tables (still the loader's, at
 * this point in boot -- the kernel doesn't get its own PML4 until M2.3). The implementation is
 * entirely x86-specific and lives in kernel/arch/x86_64/early-map.c. */
#ifndef KERNEL_INCLUDE_ARCH_EARLY_MAP_H
#define KERNEL_INCLUDE_ARCH_EARLY_MAP_H

#include "uapi/status.h"

#include <stdint.h>

/* Allocates and zeroes one 4 KiB physical page-table page, writing its physical address to
 * `outPhys`. Implemented by kernel/mm/early.c's pmmEarlyAllocPages() (passed as a callback so
 * early-map.c itself stays free of any pmm/BootInfo dependency -- it only knows about page
 * tables). Returns STATUS_ERR_NO_MEMORY if the bump allocator is exhausted. */
typedef Status (*ArchEarlyTableAllocFn)(uint64_t *outPhys);

/* Maps one 4 KiB page at `va` -> `pa` into the CPU's current CR3, walking/allocating PML4/PDPT/PD
 * entries as needed via `allocTable` (always P|W, never NX/global -- SDM Vol 3A §4.6: W is ANDed
 * and XD is ORed down the walk, so only the leaf's own flags matter) and writing the leaf as
 * X86_PTE_FLAGS_PAGE_ARRAY (pte.h). Only ever performs a 0->1 (not-present -> present) transition:
 * returns STATUS_ERR_INVALID if the leaf is already present, or if a large (PS) page sits on the
 * walk where a subtable is needed -- this mapper never overwrites, splits, or coalesces an
 * existing mapping. No TLB invalidation is needed or performed (SDM Vol 3A §4.10.4.3: no
 * translation or paging-structure-cache entry is ever created from a not-present entry, so there
 * is nothing stale to flush for a fresh mapping). Boot-time only, BSP, IF=0, not reentrant. */
Status archEarlyMapPage(uint64_t va, uint64_t pa, ArchEarlyTableAllocFn allocTable);

/* Walks the current CR3 for `va` without modifying anything. On success, `*outPa` is the physical
 * address `va` resolves to and `*outLeafSize` is the leaf's page size in bytes (0x1000, 0x200000,
 * or 0x40000000). Returns STATUS_ERR_NOT_FOUND if any level along the way is not present.
 * Boot-time only, BSP; read-only. */
Status archEarlyLookup(uint64_t va, uint64_t *outPa, uint64_t *outLeafSize);

#endif
