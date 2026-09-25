/* Portable-facing declarations for the kernel's own page tables (ARCHITECTURE §6.3, D-086..D-090,
 * ROADMAP M2.3). The implementation is entirely x86-specific and lives in
 * kernel/arch/x86_64/paging.c; kernel/mm/vmm.c is the one caller. */
#ifndef KERNEL_INCLUDE_ARCH_PAGING_H
#define KERNEL_INCLUDE_ARCH_PAGING_H

#include "bootinfo.h"
#include "uapi/status.h"
#include "vmm.h" /* VmmFlags: a portable-enough concept (writable/executable/cache type) that the
                  * arch backend takes it directly rather than a redundant arch-only flags type */

#include <stdbool.h>
#include <stdint.h>

/* SDM Vol 3A §11.12.4/D-087: programs IA32_PAT so PAT index 1 (PWT=1,PCD=0) means WC instead of
 * the power-on WT, following the MTRR-safe procedure (CR0.CD=1, WBINVD, toggle CR4.PGE to flush,
 * WRMSR, WBINVD, toggle PGE again, restore CR0), then reads the MSR back and panics on a mismatch.
 * Indices 0 and 2 (WB/UC-) keep their power-on meaning, so this is safe to run while still on the
 * loader's own page tables. Panics if CPUID doesn't report PAT support. No locks, boot-time-only,
 * not IRQ-safe (disables caching for the duration); called once, before archPagingBuildKernel(). */
void archPatInit(void);

/* Builds the kernel's own PML4 (not yet active): the HHDM over `map[0..mapCount)` (WB for
 * USABLE/LOADER_RECLAIM/KERNEL/INITRD/ACPI_RECLAIM/ACPI_NVS, WC for FRAMEBUFFER, RESERVED/BAD left
 * unmapped, the kernel text+rodata physical range carved out read-only), the kernel image at its
 * link-time VAs with per-section permissions (text R-X, rodata R--, data/bss/stacks RW-, all NX
 * except text), and eagerly allocated kernel-half PML4 entries 256-511 (D-086) -- the already-
 * built Page-array subtree (PML4 slots 448-479) is adopted by copying those entries after
 * verifying every table page under them is still PAGE_STATE_RESERVED bump-allocator memory, never
 * re-mapped from scratch. Every table page this allocates comes from pmmAllocPages(), written
 * through the (still-active, loader-owned) HHDM. Panics on any failure -- there is no partially-
 * built kernel PML4 to recover from. Does not touch CR3. No locks, boot-time-only, BSP, IF=0;
 * called once, after archPatInit() and pmmInit(). */
void archPagingBuildKernel(const BootInfo *bi, const BootMemRegion *map, uint32_t mapCount);

/* Activates the PML4 archPagingBuildKernel() built: CR4.PGE 1->0 (flushes every TLB/paging-
 * structure-cache entry, including the loader's global ones), CR3 <- the new PML4, CR4.PGE 0->1
 * (flushes again, so the new mappings' own G bit takes effect cleanly) -- SDM Vol 3A §4.10.4.1.
 * From this point on, the loader's page tables are unreachable by hardware and ordinary memory;
 * archEarlyLookup() must never be called again. vmmKernelTablesActive() (kernel/mm/vmm.c) becomes
 * true. No locks, boot-time-only, BSP; called once, right after archPagingBuildKernel(). */
void archPagingActivate(void);

/* Enables SMEP/SMAP/UMIP in CR4 when CPUID reports them (ARCHITECTURE §6.3), after first
 * asserting CR0.WP=1 and EFER.NXE=1 (the §5.4 loader contract -- panics if either is somehow
 * clear). No locks, boot-time-only, BSP; called once, after archPagingActivate(). */
void archCpuEnableProtections(void);

/* Walks the live (post-archPagingActivate) kernel PML4 and panics if: EFER.NXE/CR0.WP aren't set;
 * PML4 slots 0-255 aren't all empty; slots 256-511 aren't all present; any kernel-half entry at
 * any level has U/S set or a leaf is missing G; any leaf is both writable and executable
 * (effective W = AND of R/W down the path, effective X = NOT OR(NX) down the path); any
 * executable leaf lies outside [kernelTextStart, kernelTextEnd) or isn't 4 KiB; or the HHDM alias
 * of any kernel *text* frame is writable (rodata's HHDM alias is kept read-only too, by the same
 * shared carve-out range that keeps text's, but isn't separately re-checked here: rodata is NX,
 * so unlike text's alias, W^X itself has no stake in whether rodata's alias is writable). Logs
 * "vmm: W^X verified: ..." on success (the exact
 * line `mk/test.mk` greps for, ROADMAP M2.3's Done-when clause). No locks, boot-time-only; called
 * once from vmmInit() and again by a ktest. */
void archPagingVerifyWx(void);

/* Maps/unmaps/looks up 4 KiB leaves in the kernel PML4 -- kernel/mm/vmm.c's vmmMapKernel/
 * vmmUnmapKernel/vmmLookupKernel are thin wrappers that add the VM_KVA_BASE/END bounds check and
 * locking; these arch functions know nothing about the KVA region, only about page tables.
 * archMapPages: `va`/`pa`/`size` 4 KiB-aligned; every leaf is a fresh 0->1 transition (returns
 * STATUS_ERR_INVALID if any leaf in the range is already present) with X86_PTE_G always set;
 * STATUS_ERR_NO_MEMORY rolls back any leaf this call already wrote (never a partial mapping left
 * behind). archUnmapPages: STATUS_ERR_NOT_FOUND if any leaf in the range isn't present; never
 * frees page-table pages. Both panic if called before archPagingActivate(). No locks (the caller's
 * vmmLock covers both); IRQ-safe; may not sleep. */
Status archMapPages(uint64_t va, uint64_t pa, uint64_t size, VmmFlags flags);
Status archUnmapPages(uint64_t va, uint64_t size);
Status archLookupKernel(uint64_t va, uint64_t *outPa, VmmFlags *outFlags);

/* Invalidates every translation this CPU has cached for `[va, va+size)` (one INVLPG per 4 KiB
 * page -- M3.5 replaces the body with a real IPI shootdown to other CPUs, nothing else in vmm.c
 * changes). No locks; IRQ-safe. */
void archTlbInvalidateKernelRange(uint64_t va, uint64_t size);

/* Test/debug only (mirrors archBreakpointHits()'s existing precedent): the raw 4 KiB leaf PTE
 * value at `va` in the live kernel PML4, or 0 if not present at the 4 KiB level (including if a
 * larger leaf covers it -- callers that care already know their target is 4 KiB-mapped). No
 * locks; IRQ-safe; pure. */
uint64_t archPagingRawPte(uint64_t va);

#endif
