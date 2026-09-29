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

/* What elfRelocate() found, for the loader's log line and the tests. Filled only on BOOT_OK
 * (zeroed on entry, so a failed call leaves zeros). */
typedef struct {
    uint32_t relaSections; /* SHT_RELA sections whose target is SHF_ALLOC (the ones processed) */
    uint32_t total;        /* relocation entries in those sections, every type */
    uint32_t applied; /* entries that move a value with the slide (64 / 32S against a SEC symbol) */
    uint32_t execApplied; /* the subset of `applied` whose target section is SHF_EXECINSTR */
    uint32_t skipped;     /* NONE, PC-relative against a SEC symbol, and anything against ABS/UND */
} ElfRelocStats;

/* Rebases the kernel image `dest` (already filled by elfLoad from the same `file`) by `slide`
 * bytes, using the --emit-relocs SHT_RELA sections still present in `file` (the whole ELF, as read
 * from disk). The image was linked at img->linkBase, is about to run at img->linkBase + slide, and
 * is position-dependent (mcmodel=kernel, no PIC), so every absolute address stored in it must
 * grow by `slide`. Because the link-time content is already S+A, nothing is recomputed from
 * symbols: each affected location just gets `slide` added to what elfLoad put there.
 *
 * Two passes over every SHT_RELA whose target section (sh_info) is SHF_ALLOC (debug relocations
 * are ignored, sections are classified by type and flags, never by name): pass 1 validates the
 * whole table and writes nothing, pass 2 applies. Accepted: R_X86_64_NONE (skipped);
 * R_X86_64_64 against a section symbol (value += slide, error if it wraps); R_X86_64_32S against
 * a section symbol (sign-extended value + slide must still fit int32); R_X86_64_PC32/PLT32/PC64
 * against a section symbol (both ends move together, nothing to do); R_X86_64_64/32S/32 against an
 * ABS or UND symbol (the value does not move, skipped). Rejected with BOOT_ERR_ELF_RELOC:
 * R_X86_64_32 against a section symbol (cannot be slid), PC-relative against ABS/UND (the
 * distance changes), any other type, SHT_REL sections, a symbol in a non-alloc section or with a
 * reserved shndx, a location outside the file-backed part of a PT_LOAD or outside its target
 * section, an SHT_NOBITS target, and malformed section-header/symbol-table bounds (e_shentsize
 * 64, 1..256 sections, every offset/size overflow-checked, sh_entsize 24, sh_link naming an
 * SHT_SYMTAB). It also fails if not one 64/32S was applied into an SHF_EXECINSTR target: a kernel
 * linked without --emit-relocs would otherwise "relocate" nothing and then run with stale
 * absolute addresses. `slide` must be 4 KiB aligned and keep the image inside the kernel window,
 * else BOOT_ERR_ELF_RANGE; slide 0 still validates the table but changes no byte.
 *
 * `dest` must be img->span bytes. `stats` may be NULL. On a pass-1 failure `dest` is untouched;
 * a pass-2 failure is impossible for a table pass 1 accepted unless two entries overlap and the
 * second wraps, so after ANY failure the caller must discard `dest` (re-run elfLoad, or fall back
 * to slide 0 on a fresh copy) rather than boot it. No locks, never sleeps, boot-time or host-test
 * only (interrupts are not in play); does not allocate; no 64-bit division (i386 stage2 links no
 * libgcc). Returns BOOT_ERR_ELF_HEADER for NULL arguments or a bad ELF magic/class. */
BootStatus elfRelocate(const ElfImage *img, const uint8_t *file, uint64_t fileSize, uint8_t *dest,
                       uint64_t slide, ElfRelocStats *stats);

#endif
