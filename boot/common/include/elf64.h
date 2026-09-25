/* A from-scratch, validating ELF64 loader for kernel.elf (ARCHITECTURE §5.5/§0: no system
 * elf.h, no third-party parser). elfParse() only validates and describes the file; elfLoad()
 * copies it into an already-allocated, contiguous physical block. Kept as two phases (plus a
 * third, relocation, added by M2.6) so the caller controls the allocation between them. */
#ifndef BOOT_COMMON_ELF64_H
#define BOOT_COMMON_ELF64_H

#include <stdint.h>

#include "boot-status.h"

#define ELF_MAX_SEGMENTS 8

/* p_flags bits, ELF64 spec. Exposed (not just internal to elf.c) so paging.c can pick page-table
 * flags from ElfSegment.flags without a second copy of these constants. */
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

typedef struct {
    uint64_t vaddr, memsz, filesz, offset;
    uint32_t flags, pad;
} ElfSegment;

typedef struct {
    uint64_t entry;    /* e_entry, link-time VA */
    uint64_t linkBase; /* lowest PT_LOAD p_vaddr */
    uint64_t span;     /* alignUp(max(p_vaddr+p_memsz), 4096) - linkBase */
    uint32_t segCount;
    ElfSegment segs[ELF_MAX_SEGMENTS];
} ElfImage;

/* Validates `file` (fileSize bytes) as a bongOS kernel image: ELF64, x86_64, ET_EXEC, at most
 * ELF_MAX_SEGMENTS PT_LOAD segments, each page-aligned, W^X, inside the kernel window
 * (BOOTINFO_KERNEL_WINDOW_BASE/END), ascending and page-disjoint, with the entry point inside an
 * executable segment. Copies headers with bootMemcpy rather than pointer-casting (alignment
 * safety; avoids UBSan reports in host tests). Does not allocate memory. No locks, boot-time or
 * host-test only. */
BootStatus elfParse(const uint8_t *file, uint64_t fileSize, ElfImage *out);

/* Copies `img`'s segments from `file` into `dest`, which must be `img->span` bytes, already
 * allocated at some physical base (the caller records that base separately: `phys(va) = physBase
 * + (va - img->linkBase)`). Zeroes the span first, then copies each segment's filesz bytes at its
 * vaddr-relative offset (the memsz-filesz tail is therefore left zero, satisfying .bss). No
 * locks, boot-time or host-test only. */
BootStatus elfLoad(const ElfImage *img, const uint8_t *file, uint8_t *dest);

#endif
