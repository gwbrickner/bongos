/* Page-table entry bits (SDM Vol 3A §4.5, 4-level paging) for the kernel side's early mapper
 * (early-map.c). Deliberately not shared with boot/common/include/paging.h's PT_* macros -- the
 * loader and the kernel are built by different toolchains/link units, and ARCHITECTURE §4 keeps
 * x86 specifics under kernel/arch/x86_64/ rather than pulling in loader-only headers. */
#ifndef KERNEL_ARCH_X86_64_PTE_H
#define KERNEL_ARCH_X86_64_PTE_H

#define X86_PTE_P         (1ULL << 0)
#define X86_PTE_W         (1ULL << 1)
#define X86_PTE_US        (1ULL << 2)
#define X86_PTE_PWT       (1ULL << 3) /* PAT index bit 0 (D-087: WB=0, WC=1 at index 0/1) */
#define X86_PTE_PCD       (1ULL << 4) /* PAT index bit 1 -- unused (no UC mapping exists yet) */
#define X86_PTE_PS        (1ULL << 7)
#define X86_PTE_G         (1ULL << 8)
#define X86_PTE_NX        (1ULL << 63)
#define X86_PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define X86_PTE_SIZE_4K (1ULL << 12)
#define X86_PTE_SIZE_2M (1ULL << 21)
#define X86_PTE_SIZE_1G (1ULL << 30)

/* The Page array's leaf mapping (D-079): RW-/NX/global, WB (PWT=PCD=0 selects PAT index 0 --
 * D-087's kernel PAT layout keeps index 0 = WB, same as the firmware's power-on default, so this
 * mapping's meaning is unchanged by M2.3's PAT reprogramming). Non-leaf (PML4E/PDPTE/PDE-as-
 * pointer) entries are always P|W only, same convention as the loader's ptGetOrAllocTable(). */
#define X86_PTE_FLAGS_PAGE_ARRAY (X86_PTE_P | X86_PTE_W | X86_PTE_NX | X86_PTE_G)

#endif
