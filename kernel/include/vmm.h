/* The kernel virtual memory manager (ARCHITECTURE §6.1/§6.3, D-086..D-090, ROADMAP M2.3):
 * vmmInit() builds and switches to the kernel's own page tables (HHDM, per-section kernel image
 * permissions, eagerly allocated kernel-half PML4 slots 256-511), enables SMEP/SMAP/UMIP, and
 * brings up the kernel virtual area (KVA) allocator. vmmMapKernel()/vmmUnmapKernel() are the only
 * way anything maps memory into the KVA region after that -- used directly here (the framebuffer's
 * WC mapping is built in by vmmInit() itself, not through these) and by M2.4's vmalloc. The real
 * per-arch work (PML4 build, CR3 switch, PAT, CR4 protections, W^X verification) lives in
 * kernel/arch/x86_64/paging.c behind kernel/include/arch/paging.h (ARCHITECTURE §4). */
#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include "bootinfo.h"
#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

/* ARCHITECTURE §6.1: the kernel virtual area -- vmalloc, thread stacks (with guard pages), MMIO,
 * fixmaps. M2.3 only hands out plain RW/RO/WC 4 KiB mappings from it (vmmMapKernel); nothing here
 * builds VmRegion/AddressSpace machinery, which belongs to M4. */
#define VM_KVA_BASE 0xFFFFC00000000000ULL
#define VM_KVA_END  0xFFFFE00000000000ULL /* exclusive */

typedef uint32_t VmmFlags;
#define VMM_WRITE       (1u << 0)
#define VMM_EXEC        (1u << 1) /* rejected with STATUS_ERR_UNSUPPORTED in M2.3 (D-088) */
#define VMM_CACHE_WB    (0u << 2)
#define VMM_CACHE_WC    (1u << 2)
#define VMM_CACHE_MASK  (1u << 2)
#define VMM_FLAGS_VALID (VMM_WRITE | VMM_EXEC | VMM_CACHE_MASK)

/* Builds the kernel's own page tables over `map[0..mapCount)` (the kernel's snapshot of the
 * BootInfo memory map, kernel/core/main.c's kernelBootMemMap()), switches CR3 to them, enables
 * SMEP/SMAP/UMIP when present, verifies W^X (panics otherwise), and initializes the KVA allocator.
 * Must run after pmmInit() (every page-table page it allocates comes from pmmAllocPages) and
 * before anything calls vmmMapKernel/vmmUnmapKernel/vmmKvaAlloc or pmmReclaimLoaderMemory(). Boot-
 * time only, BSP, IF=0, not reentrant; called once from kernelMain. */
void vmmInit(const BootInfo *bi, const BootMemRegion *map, uint32_t mapCount);

/* True once vmmInit() has switched CR3 to the kernel's own tables (i.e. the loader's identity/HHDM
 * mapping is no longer in use). pmmReclaimLoaderMemory() requires this before it will run. No
 * locks; pure. */
bool vmmKernelTablesActive(void);

/* Maps `[va, va+size)` (4 KiB-aligned, non-empty, entirely inside [VM_KVA_BASE, VM_KVA_END)) to
 * `[pa, pa+size)` with `flags` (VMM_WRITE/VMM_CACHE_WB|WC; VMM_EXEC returns
 * STATUS_ERR_UNSUPPORTED -- the module loader gets its own executable-range API later). Every leaf
 * is global, 4 KiB, and a fresh 0->1 transition: returns STATUS_ERR_INVALID if any page in the
 * range is already mapped, out of the KVA region, misaligned, past this CPU's MAXPHYADDR (or
 * `pa+size` overflows), requests VMM_WRITE over any part of the kernel's own text+rodata physical
 * range (D-090 point g: never a second, KVA-side writable alias of read-only executable memory),
 * or requests a cache type that conflicts with that physical page's existing HHDM alias (SDM Vol
 * 3A §11.12.4: mapping one physical page with two memory types is unsupported). Returns
 * STATUS_ERR_NO_MEMORY if a table page can't be allocated, after rolling back any leaves this call
 * already wrote. Locks: vmmLock
 * (IRQ-disable only, D-088 -- mirrors the pmm's D-081 lock, same single-CPU/IF=0 justification).
 * Lock order: vmmLock -> pmmLock. IRQ-safe: yes. May sleep: no. Panics if called before vmmInit().
 */
Status vmmMapKernel(uint64_t va, uint64_t pa, uint64_t size, VmmFlags flags);

/* Unmaps `[va, va+size)` (same alignment/bounds rules as vmmMapKernel), invalidating the TLB for
 * every page. Returns STATUS_ERR_NOT_FOUND if any page in the range isn't currently mapped, or
 * STATUS_ERR_INVALID for a bad range. Never frees the underlying physical frames or the page
 * tables themselves -- the caller owns the frames; page-table pages are never freed in M2.3 (no
 * safe point to reclaim one without a shootdown, which arrives with SMP, M3.4/M3.5). Locks:
 * vmmLock. IRQ-safe: yes. May sleep: no. */
Status vmmUnmapKernel(uint64_t va, uint64_t size);

/* Looks up the current 4 KiB-leaf mapping at `va` (must itself be 4 KiB-aligned). On success,
 * `*outPa` and `*outFlags` describe the leaf and this returns STATUS_OK; returns
 * STATUS_ERR_NOT_FOUND if `va` isn't mapped *as a 4 KiB leaf* -- unlike vmmMapKernel/
 * vmmUnmapKernel, this isn't restricted to the KVA region (it also works against KVA-region VAs
 * and against the framebuffer's HHDM mapping, both always 4 KiB), but a VA covered by one of the
 * kernel's own 2 MiB/1 GiB HHDM leaves (most of the HHDM, D-086) reports NOT_FOUND rather than
 * resolving through the large leaf. No locks beyond a brief vmmLock hold; IRQ-safe; pure with
 * respect to visible kernel state. */
Status vmmLookupKernel(uint64_t va, uint64_t *outPa, VmmFlags *outFlags);

/* Reserves `size` (4 KiB-aligned, nonzero) bytes of KVA space, with an unmapped guard page on
 * each side, and returns the usable VA via `*outVa`. The returned range is *not* mapped to
 * anything -- pair with vmmMapKernel. Returns STATUS_ERR_NO_MEMORY if the KVA space is exhausted,
 * STATUS_ERR_INVALID for a bad size. Locks: vmmLock. IRQ-safe: yes. May sleep: no. */
Status vmmKvaAlloc(uint64_t size, uint64_t *outVa);

/* Returns a `[va, va+size)` range (plus its guard pages) that vmmKvaAlloc() handed out, `size`
 * exactly matching that call. Any misuse (an unrecognized range, a double free) is a kernel bug
 * and panics via panicBug() (D-082's pattern) -- and so, separately, does a *correct* call that
 * can't be recorded because the KVA extent table is already full of other free ranges
 * (KVA_MAX_EXTENTS=512, D-088/D-091: a recorded, accepted capacity limit, not misuse, but this
 * function has no way to report it other than panicking, since it returns nothing). No current
 * M2.3 caller comes anywhere near that limit. Caller's responsibility to have already unmapped
 * anything still mapped in the range. Locks: vmmLock. IRQ-safe: yes. May sleep: no. */
void vmmKvaFree(uint64_t va, uint64_t size);

#endif
