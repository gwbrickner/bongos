/* Memory-map conversion (ARCHITECTURE §5.5 step 10/§6.1's HHDM contract, D-060). Kept
 * EFI-header-free: `memMapEfiTypeToBootMem` and `memMapNormalize` take plain integers/structs
 * rather than EFI_MEMORY_DESCRIPTOR, so this file (and its host tests, boot_memmap_test.c) never
 * need the boot/uefi/include/efi/ headers. boot/uefi/handoff.c is what walks the real EFI map and
 * calls these. */
#ifndef BOOT_COMMON_MEMMAP_H
#define BOOT_COMMON_MEMMAP_H

#include <stdint.h>

#include "boot-status.h"
#include "bootinfo.h"

/* UEFI Spec §7.2's EFI_MEMORY_SP attribute bit ("special purpose", e.g. persistent-memory-backed
 * conventional memory): D-060 treats a Conventional descriptor with this bit as RESERVED rather
 * than USABLE. */
#define MEM_MAP_EFI_ATTRIBUTE_SP 0x40000ULL

/* Maps one UEFI EFI_MEMORY_TYPE value (0-15, or any later/OEM value >= 16) plus its Attribute
 * bitmask to a BootMemType, per D-060's table. No locks, boot-time or host-test only; pure. */
uint32_t memMapEfiTypeToBootMem(uint32_t efiType, uint64_t attribute);

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type; /* a BootMemType value for memMapNormalize; a raw EFI_MEMORY_TYPE value (only
                      1 or 2 matter) for memMapCheckOverlay */
} MemMapInput;

/* Rank-sweep normalization (D-060): merges `in` (nIn entries -- the EFI map already passed
 * through memMapEfiTypeToBootMem, plus the loader's own KERNEL/LOADER_RECLAIM/etc. overlay
 * entries) into `out` (capacity outCap), producing a sorted, non-overlapping, page-aligned array
 * with adjacent same-type regions coalesced and holes preserved (never invented as RESERVED).
 * `scratchPoints` must have room for at least 2*nIn uint64_t entries; the caller owns its
 * lifetime (no allocation happens in here). Returns BOOT_ERR_MEMMAP_CAPACITY if more than outCap
 * output regions would be needed. No locks, boot-time or host-test only; pure (no
 * syscalls/allocation). */
BootStatus memMapNormalize(const MemMapInput *in, uint32_t nIn, BootMemRegion *out, uint32_t outCap,
                           uint32_t *nOut, uint64_t *scratchPoints);

/* Checks the loader's own overlay contract (D-060): every LOADER_RECLAIM/KERNEL/INITRD
 * allocation must lie entirely inside a single EFI descriptor of type EfiLoaderCode (1) or
 * EfiLoaderData (2) -- never spanning descriptors, and never inside any other EFI type. Returns
 * BOOT_ERR_MEMMAP_OVERLAY if not. A zero-length range always passes. No locks, boot-time or
 * host-test only; pure. */
BootStatus memMapCheckOverlay(uint64_t base, uint64_t length, const MemMapInput *efiRegions,
                              uint32_t nEfi);

#endif
