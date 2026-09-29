/* Kernel image relocation for KASLR (ARCHITECTURE §5.5/§6.6, M2.6, D-120). See elf64.h for the
 * contract. Reads the SHT_RELA sections that `ld.lld --emit-relocs` leaves in kernel.elf (the
 * loader's file buffer holds the whole ELF, not just the PT_LOADs) and adds the slide to every
 * absolute address in the already-loaded image. Like elf.c it copies headers with bootMemcpy into
 * local structs (never pointer-casts into the file buffer: alignment safety, UBSan-clean host
 * tests) and never uses 64-bit `/` or `%` (D-065: the i386 stage2 links no libgcc). */
#include "include/bootinfo.h"
#include "include/bootmem.h"
#include "include/elf64.h"

#define SHT_SYMTAB 2u
#define SHT_RELA   4u
#define SHT_NOBITS 8u
#define SHT_REL    9u

#define SHF_ALLOC     0x2ULL
#define SHF_EXECINSTR 0x4ULL

#define SHN_LORESERVE 0xFF00u
#define SHN_ABS       0xFFF1u

#define R_X86_64_NONE  0u
#define R_X86_64_64    1u
#define R_X86_64_PC32  2u
#define R_X86_64_PLT32 4u
#define R_X86_64_32    10u
#define R_X86_64_32S   11u
#define R_X86_64_PC64  24u

#define ELF_SHDR_SIZE      64u
#define ELF_RELA_SIZE      24u
#define ELF_SYM_SIZE       24u
#define RELOC_MAX_SECTIONS 256u
#define RELOC_PAGE_SIZE    4096ULL

typedef struct {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} RawSectionHeader;
_Static_assert(sizeof(RawSectionHeader) == ELF_SHDR_SIZE, "ELF64 section header must be 64 bytes");

typedef struct {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} RawSymbol;
_Static_assert(sizeof(RawSymbol) == ELF_SYM_SIZE, "ELF64 symbol must be 24 bytes");

typedef struct {
    uint64_t offset;
    uint64_t info;
    uint64_t addend; /* signed on disk; never used (we add the slide to what is already there) */
} RawRela;
_Static_assert(sizeof(RawRela) == ELF_RELA_SIZE, "ELF64 Rela must be 24 bytes");

/* Per-section facts needed when a symbol or a relocation names a section by index. */
#define SEC_ALLOC  1u
#define SEC_NOBITS 2u

typedef enum { SYM_SEC, SYM_ABS, SYM_UND } SymKind;

typedef struct {
    const ElfImage *img;
    const uint8_t *file;
    uint64_t fileSize;
    uint8_t *dest;
    uint64_t slide;
    uint64_t shoff;
    uint32_t shnum;
    uint8_t secFlags[RELOC_MAX_SECTIONS];
} RelocCtx;

static uint64_t readU64(const uint8_t *p) {
    uint64_t v;
    bootMemcpy(&v, p, sizeof(v));
    return v;
}

static uint16_t readU16(const uint8_t *p) {
    uint16_t v;
    bootMemcpy(&v, p, sizeof(v));
    return v;
}

/* True when [off, off+size) lies inside a `limit`-byte object. Overflow-safe. */
static int rangeFits(uint64_t off, uint64_t size, uint64_t limit) {
    return off <= limit && size <= limit - off;
}

/* Section header `idx` (< ctx->shnum, whose table bounds were checked up front). */
static void readSection(const RelocCtx *ctx, uint32_t idx, RawSectionHeader *out) {
    bootMemcpy(out, ctx->file + ctx->shoff + (uint64_t)idx * ELF_SHDR_SIZE, sizeof(*out));
}

/* Locates the width-byte location at virtual address `va` in the loaded image: it must sit inside
 * the target section [tAddr, tAddr+tSize) and inside the file-backed part of one PT_LOAD (the
 * .bss-style memsz-filesz tail holds nothing that came from the file, so a reloc there is a lie),
 * and inside `dest`. Stores the offset into `dest` in *outOff and the offset of the same bytes in
 * `file` in *outFileOff; returns a negative value on failure. */
static int locateReloc(const RelocCtx *ctx, uint64_t va, uint32_t width, uint64_t tAddr,
                       uint64_t tSize, uint64_t *outOff, uint64_t *outFileOff) {
    if (va < tAddr || !rangeFits(va - tAddr, width, tSize)) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->img->segCount; i++) {
        const ElfSegment *s = &ctx->img->segs[i];
        if (va >= s->vaddr && rangeFits(va - s->vaddr, width, s->filesz)) {
            uint64_t off = va - ctx->img->linkBase; /* vaddr >= linkBase: linkBase is the lowest */
            if (!rangeFits(off, width, ctx->img->span) || s->offset > ctx->fileSize ||
                !rangeFits(va - s->vaddr, width, ctx->fileSize - s->offset)) {
                return -1; /* overflow-safe: never forms s->offset + (va - s->vaddr) unchecked */
            }
            *outOff = off;
            *outFileOff = s->offset + (va - s->vaddr);
            return 0;
        }
    }
    return -1;
}

/* Validates (apply == 0) or applies (apply == 1) one 64/32S relocation at dest+off. The two share
 * this function so pass 2 can never disagree with pass 1 about what is legal. The location must
 * still hold exactly the bytes elfLoad copied from file+fileOff: in pass 1 that proves `dest` is
 * this file's freshly loaded image (not an already-slid one, which would be slid twice); in pass 2
 * it catches a second entry naming bytes an earlier entry already rewrote (a duplicate or an
 * overlap), which pass 1, reading the untouched image, cannot see. */
static BootStatus slideLocation(const RelocCtx *ctx, uint32_t type, uint64_t off, uint64_t fileOff,
                                int apply) {
    uint8_t *loc = ctx->dest + off;
    uint32_t width = (type == R_X86_64_64) ? 8u : 4u;
    for (uint32_t i = 0; i < width; i++) {
        if (loc[i] != ctx->file[fileOff + i]) {
            return BOOT_ERR_ELF_RELOC;
        }
    }
    if (type == R_X86_64_64) {
        uint64_t v = readU64(loc);
        uint64_t nv = v + ctx->slide;
        if (nv < v) {
            return BOOT_ERR_ELF_RELOC;
        }
        if (apply) {
            bootMemcpy(loc, &nv, sizeof(nv));
        }
        return BOOT_OK;
    }
    /* R_X86_64_32S: the stored 32 bits are a sign-extended 64-bit address (kernel-window
     * addresses have bit 31 set); after sliding they must still be representable that way.
     * slide <= 0x20000000 (checked by the caller), so the int64 sum cannot overflow. */
    uint32_t raw;
    bootMemcpy(&raw, loc, sizeof(raw));
    int64_t sum = (int64_t)(int32_t)raw + (int64_t)ctx->slide;
    if (sum < -(int64_t)0x80000000LL || sum > (int64_t)0x7FFFFFFFLL) {
        return BOOT_ERR_ELF_RELOC;
    }
    if (apply) {
        uint32_t nv = (uint32_t)(int32_t)sum;
        bootMemcpy(loc, &nv, sizeof(nv));
    }
    return BOOT_OK;
}

/* One walk over every relevant SHT_RELA. apply == 0: validate and count (stats), write nothing.
 * apply == 1: re-derive the same decisions and write. */
static BootStatus relocWalk(const RelocCtx *ctx, int apply, ElfRelocStats *stats) {
    for (uint32_t si = 1; si < ctx->shnum; si++) {
        RawSectionHeader rs;
        readSection(ctx, si, &rs);
        if (rs.type == SHT_REL) {
            return BOOT_ERR_ELF_RELOC; /* x86_64 toolchains emit RELA; REL would need addends read
                                          from the image, which we do not support */
        }
        if (rs.type != SHT_RELA) {
            continue;
        }
        if (rs.info >= ctx->shnum) {
            return BOOT_ERR_ELF_RELOC;
        }
        if ((ctx->secFlags[rs.info] & SEC_ALLOC) == 0) {
            continue; /* .rela.debug_*, .rela.comment...: nothing the running image contains */
        }
        if ((ctx->secFlags[rs.info] & SEC_NOBITS) != 0) {
            return BOOT_ERR_ELF_RELOC; /* nothing in the file to relocate; NOBITS is zero-fill */
        }
        if (rs.entsize != ELF_RELA_SIZE || !rangeFits(rs.offset, rs.size, ctx->fileSize)) {
            return BOOT_ERR_ELF_RELOC;
        }
        uint32_t rem;
        uint64_t relaCount = bootDivMod64(rs.size, ELF_RELA_SIZE, &rem);
        if (rem != 0) {
            return BOOT_ERR_ELF_RELOC;
        }

        RawSectionHeader ts; /* the section being patched */
        readSection(ctx, rs.info, &ts);
        if (ts.addr + ts.size < ts.addr) {
            return BOOT_ERR_ELF_RELOC;
        }

        if (rs.link == 0 || rs.link >= ctx->shnum) {
            return BOOT_ERR_ELF_RELOC;
        }
        RawSectionHeader ss; /* its symbol table */
        readSection(ctx, rs.link, &ss);
        if (ss.type != SHT_SYMTAB || ss.entsize != ELF_SYM_SIZE ||
            !rangeFits(ss.offset, ss.size, ctx->fileSize)) {
            return BOOT_ERR_ELF_RELOC;
        }
        uint64_t symCount = bootDivMod64(ss.size, ELF_SYM_SIZE, &rem);
        if (rem != 0) {
            return BOOT_ERR_ELF_RELOC;
        }

        if (!apply) {
            stats->relaSections++;
        }
        for (uint64_t ri = 0; ri < relaCount; ri++) {
            RawRela r;
            bootMemcpy(&r, ctx->file + rs.offset + ri * ELF_RELA_SIZE, sizeof(r));
            uint32_t type = (uint32_t)r.info;
            uint32_t symIdx = (uint32_t)(r.info >> 32);
            if (!apply) {
                stats->total++;
            }
            if (type == R_X86_64_NONE) {
                if (!apply) {
                    stats->skipped++;
                }
                continue;
            }

            if (symIdx >= symCount) {
                return BOOT_ERR_ELF_RELOC;
            }
            RawSymbol sym;
            bootMemcpy(&sym, ctx->file + ss.offset + (uint64_t)symIdx * ELF_SYM_SIZE, sizeof(sym));
            SymKind kind;
            if (sym.shndx == 0) {
                kind = SYM_UND;
            } else if (sym.shndx == SHN_ABS) {
                kind = SYM_ABS;
            } else if (sym.shndx >= SHN_LORESERVE || sym.shndx >= ctx->shnum ||
                       (ctx->secFlags[sym.shndx] & SEC_ALLOC) == 0) {
                return BOOT_ERR_ELF_RELOC; /* COMMON, XINDEX, out of range, or non-alloc section */
            } else {
                kind = SYM_SEC;
            }

            switch (type) {
                case R_X86_64_64:
                case R_X86_64_32S: {
                    if (kind != SYM_SEC) {
                        if (!apply) {
                            stats->skipped++;
                        }
                        break; /* an absolute value: does not move with the image */
                    }
                    uint32_t width = (type == R_X86_64_64) ? 8u : 4u;
                    uint64_t off, fileOff;
                    if (locateReloc(ctx, r.offset, width, ts.addr, ts.size, &off, &fileOff) != 0) {
                        return BOOT_ERR_ELF_RELOC;
                    }
                    BootStatus st = slideLocation(ctx, type, off, fileOff, apply);
                    if (st != BOOT_OK) {
                        return st;
                    }
                    if (!apply) {
                        stats->applied++;
                        if ((ts.flags & SHF_EXECINSTR) != 0) {
                            stats->execApplied++;
                        }
                    }
                    break;
                }
                case R_X86_64_32:
                    if (kind == SYM_SEC) {
                        return BOOT_ERR_ELF_RELOC; /* a 32-bit zero-extended address cannot hold a
                                                      kernel-window address, let alone slide it */
                    }
                    if (!apply) {
                        stats->skipped++;
                    }
                    break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32:
                case R_X86_64_PC64:
                    if (kind != SYM_SEC) {
                        return BOOT_ERR_ELF_RELOC; /* distance to a fixed address changes */
                    }
                    if (!apply) {
                        stats->skipped++; /* both ends move together: nothing to do */
                    }
                    break;
                default:
                    return BOOT_ERR_ELF_RELOC;
            }
        }
    }
    return BOOT_OK;
}

BootStatus elfRelocate(const ElfImage *img, const uint8_t *file, uint64_t fileSize, uint8_t *dest,
                       uint64_t slide, ElfRelocStats *stats) {
    if (stats != NULL) {
        bootMemset(stats, 0, sizeof(*stats));
    }
    if (img == NULL || file == NULL || dest == NULL || fileSize < 64 || file[0] != 0x7F ||
        file[1] != 'E' || file[2] != 'L' || file[3] != 'F' || file[4] != 2 /* ELFCLASS64 */ ||
        file[5] != 1 /* ELFDATA2LSB */) {
        return BOOT_ERR_ELF_HEADER;
    }
    /* The slid image must stay page-aligned and inside the kernel window (ARCHITECTURE §6.1); this
     * also bounds `slide` well below 2^31, which slideLocation()'s int64 arithmetic relies on. */
    uint64_t windowRoom = BOOTINFO_KERNEL_WINDOW_END - img->linkBase;
    if ((slide & (RELOC_PAGE_SIZE - 1)) != 0 || img->linkBase < BOOTINFO_KERNEL_WINDOW_BASE ||
        img->linkBase >= BOOTINFO_KERNEL_WINDOW_END || img->span > windowRoom ||
        slide > windowRoom - img->span) {
        return BOOT_ERR_ELF_RANGE;
    }

    RelocCtx ctx;
    ctx.img = img;
    ctx.file = file;
    ctx.fileSize = fileSize;
    ctx.dest = dest;
    ctx.slide = slide;
    ctx.shoff = readU64(file + 0x28);
    uint16_t shentsize = readU16(file + 0x3A);
    ctx.shnum = readU16(file + 0x3C);
    if (shentsize != ELF_SHDR_SIZE || ctx.shnum < 1 || ctx.shnum > RELOC_MAX_SECTIONS ||
        !rangeFits(ctx.shoff, (uint64_t)ctx.shnum * ELF_SHDR_SIZE, fileSize)) {
        return BOOT_ERR_ELF_RELOC;
    }
    for (uint32_t i = 0; i < ctx.shnum; i++) {
        RawSectionHeader sh;
        readSection(&ctx, i, &sh);
        uint8_t f = 0;
        if ((sh.flags & SHF_ALLOC) != 0) {
            f |= SEC_ALLOC;
        }
        if (sh.type == SHT_NOBITS) {
            f |= SEC_NOBITS;
        }
        ctx.secFlags[i] = f;
    }

    ElfRelocStats local;
    bootMemset(&local, 0, sizeof(local));
    BootStatus st = relocWalk(&ctx, 0, &local);
    if (st != BOOT_OK) {
        return st;
    }
    if (local.execApplied == 0) {
        return BOOT_ERR_ELF_RELOC; /* no relocation touched code: not linked with --emit-relocs */
    }
    if (slide != 0) {
        ElfRelocStats scratch;
        st = relocWalk(&ctx, 1, &scratch);
        if (st != BOOT_OK) {
            return st;
        }
    }
    if (stats != NULL) {
        *stats = local;
    }
    return BOOT_OK;
}
