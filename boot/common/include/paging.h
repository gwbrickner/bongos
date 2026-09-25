/* The loader's page-table builder (ARCHITECTURE §5.4/§6.1/§6.3). Builds the 4-level page
 * tables the kernel is handed at entry: the HHDM, the kernel image mapping, and the trampoline's
 * identity page. Runs before ExitBootServices, where every physical address is still directly
 * usable as a pointer (bootPhysToPtr, bootmem.h). */
#ifndef BOOT_COMMON_PAGING_H
#define BOOT_COMMON_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#include "boot-status.h"
#include "elf64.h"

/* Page-table entry bits, SDM Vol 3A §4.5 (4-level paging). */
#define PT_P         (1ULL << 0)
#define PT_W         (1ULL << 1)
#define PT_PS        (1ULL << 7)
#define PT_G         (1ULL << 8)
#define PT_NX        (1ULL << 63)
#define PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PT_SIZE_4K (1ULL << 12)
#define PT_SIZE_2M (1ULL << 21)
#define PT_SIZE_1G (1ULL << 30)

/* Leaf flag combinations (the table in the M1.3 design doc). Non-leaf entries (PML4E/PDPTE/PDE
 * used as a pointer to the next level) always get P|W only -- never NX/global -- written directly
 * by ptGetOrAllocTable() in paging.c, not through these macros. */
/* Uniformly RW-/NX: the HHDM also aliases the kernel's own text/rodata as writable, which is a
 * known gap (out of scope here) -- making that alias read-only is M2.3's job, once something
 * actually needs a non-uniform HHDM. */
#define PT_FLAGS_HHDM       (PT_P | PT_W | PT_NX | PT_G)
#define PT_FLAGS_KERNEL_RX  (PT_P | PT_G)
#define PT_FLAGS_KERNEL_RO  (PT_P | PT_G | PT_NX)
#define PT_FLAGS_KERNEL_RW  (PT_P | PT_W | PT_G | PT_NX)
#define PT_FLAGS_TRAMPOLINE (PT_P) /* R-X, identity, deliberately not global (§5.4) */

typedef struct {
    uint64_t poolPhys;
    uint32_t poolPages;
    uint32_t poolUsed;
    bool has1G;
    uint64_t pml4Phys;
} PtBuilder;

/* Zeroes the whole `poolPages`-page pool at `poolPhys` and takes its first page as the PML4.
 * `has1G` gates 1 GiB leaf mappings (CPUID 0x80000001 EDX bit 26, PDPE1GB) -- without it,
 * ptMapRange() falls back to 2 MiB pages. No locks, boot-time or host-test only. */
BootStatus ptInit(PtBuilder *b, uint64_t poolPhys, uint32_t poolPages, bool has1G);

/* Maps `size` bytes at `va` to `pa`, using the largest page size the alignment/size/has1G allow
 * when `allowLarge` is set (never straddling a leaf boundary), or 4 KiB pages when it isn't
 * (§6.3: the kernel image is always 4K-mapped). Every leaf gets exactly `leafFlags`. `size`, `va`,
 * and `pa` must already be 4 KiB-aligned -- returns BOOT_ERR_PT_UNALIGNED if not (a misaligned
 * `size` would otherwise underflow the loop counter and keep mapping until the pool is exhausted).
 * Returns BOOT_ERR_PT_CONFLICT if a mapping already covers part of the range (this builder never
 * overwrites or coalesces an existing entry) or BOOT_ERR_NO_MEMORY if the pool runs out. No locks,
 * boot-time or host-test only. */
BootStatus ptMapRange(PtBuilder *b, uint64_t va, uint64_t pa, uint64_t size, uint64_t leafFlags,
                      bool allowLarge);

/* Maps every PT_LOAD segment of `img` at `img->segs[i].vaddr + slide`, backed by the contiguous
 * physical block starting at `physBase` (`phys(va) = physBase + (va - img->linkBase)`), always as
 * 4 KiB pages, with per-segment R-X/R--/RW- flags picked from ElfSegment.flags. `slide` is 0 until
 * M2.6 adds KASLR relocation. No locks, boot-time or host-test only. */
BootStatus ptMapElfImage(PtBuilder *b, const ElfImage *img, uint64_t physBase, uint64_t slide);

/* Walks the page tables for `va` and reports the physical address and the leaf entry's raw flag
 * bits (address bits masked out) it resolves to. Used both by the loader's own pre-jump
 * self-check and by host tests. Returns BOOT_ERR_NOT_MAPPED if any level along the way is not
 * present. No locks, boot-time or host-test only; read-only. */
BootStatus ptLookup(const PtBuilder *b, uint64_t va, uint64_t *pa, uint64_t *leafFlags);

#endif
