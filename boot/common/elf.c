/* ELF64 loader for kernel.elf. See elf64.h. Own struct definitions from the ELF64 spec (no system
 * elf.h, ARCHITECTURE §0); every multi-byte value is copied with bootMemcpy into a local struct
 * whose field order matches the on-disk layout exactly (verified below with _Static_assert),
 * rather than pointer-cast, so an unaligned `file` buffer never faults and UBSan stays quiet in
 * host tests. */
#include "include/elf64.h"

#include "include/bootinfo.h"
#include "include/bootmem.h"

/* e_ident indices and required values, ELF64 spec §"ELF Identification". */
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1

#define ET_EXEC   2
#define EM_X86_64 62

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_TLS     7

#define ELF_PAGE_SIZE 4096ULL

typedef struct {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} RawElfHeader;
_Static_assert(sizeof(RawElfHeader) == 64, "ELF64 header must be 64 bytes");

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} RawProgramHeader;
_Static_assert(sizeof(RawProgramHeader) == 56, "ELF64 program header must be 56 bytes");

BootStatus elfParse(const uint8_t *file, uint64_t fileSize, ElfImage *out) {
    if (file == NULL || out == NULL || fileSize < sizeof(RawElfHeader)) {
        return BOOT_ERR_ELF_HEADER;
    }
    RawElfHeader hdr;
    bootMemcpy(&hdr, file, sizeof(hdr));

    if (hdr.ident[0] != 0x7F || hdr.ident[1] != 'E' || hdr.ident[2] != 'L' || hdr.ident[3] != 'F') {
        return BOOT_ERR_ELF_HEADER;
    }
    if (hdr.ident[EI_CLASS] != ELFCLASS64 || hdr.ident[EI_DATA] != ELFDATA2LSB ||
        hdr.ident[EI_VERSION] != EV_CURRENT) {
        return BOOT_ERR_ELF_HEADER;
    }
    if (hdr.type != ET_EXEC || hdr.machine != EM_X86_64 || hdr.version != EV_CURRENT) {
        return BOOT_ERR_ELF_HEADER;
    }
    if (hdr.phentsize != sizeof(RawProgramHeader) || hdr.phnum < 1 || hdr.phnum > 16) {
        return BOOT_ERR_ELF_PHDR;
    }
    /* Overflow-checked: hdr.phoff + hdr.phnum * phentsize <= fileSize. phnum <= 16 and
     * phentsize == 56 keep the product itself well under 2^64, so only the final addition needs
     * a check. */
    uint64_t phTableBytes = (uint64_t)hdr.phnum * sizeof(RawProgramHeader);
    if (hdr.phoff > fileSize || phTableBytes > fileSize - hdr.phoff) {
        return BOOT_ERR_ELF_PHDR;
    }

    out->segCount = 0;
    out->entry = hdr.entry;
    out->linkBase = 0;
    out->span = 0;
    int haveLinkBase = 0;
    uint64_t maxEnd = 0;

    for (uint16_t i = 0; i < hdr.phnum; i++) {
        RawProgramHeader ph;
        bootMemcpy(&ph, file + hdr.phoff + (uint64_t)i * sizeof(RawProgramHeader), sizeof(ph));

        if (ph.type == PT_INTERP || ph.type == PT_DYNAMIC || ph.type == PT_TLS) {
            return BOOT_ERR_ELF_SEGMENT;
        }
        if (ph.type != PT_LOAD) {
            continue;
        }
        if (out->segCount >= ELF_MAX_SEGMENTS) {
            return BOOT_ERR_ELF_SEGMENT;
        }
        if (ph.memsz == 0 || ph.memsz < ph.filesz) {
            return BOOT_ERR_ELF_SEGMENT;
        }
        if (ph.offset > fileSize || ph.filesz > fileSize - ph.offset) {
            return BOOT_ERR_ELF_SEGMENT;
        }
        if ((ph.vaddr & (ELF_PAGE_SIZE - 1)) != 0) {
            return BOOT_ERR_ELF_SEGMENT;
        }
        if ((ph.flags & ELF_PF_R) == 0) {
            return BOOT_ERR_ELF_SEGMENT;
        }
        if ((ph.flags & (ELF_PF_W | ELF_PF_X)) == (ELF_PF_W | ELF_PF_X)) {
            return BOOT_ERR_ELF_WX;
        }
        /* Reject vaddr at/beyond the window *before* computing END - vaddr: if vaddr >= END that
         * subtraction underflows (unsigned wraparound) to a huge value and the memsz check below
         * would wrongly pass. Order matters here. */
        if (ph.vaddr < BOOTINFO_KERNEL_WINDOW_BASE || ph.vaddr >= BOOTINFO_KERNEL_WINDOW_END ||
            ph.memsz > BOOTINFO_KERNEL_WINDOW_END - ph.vaddr) {
            return BOOT_ERR_ELF_RANGE;
        }
        uint64_t end = ph.vaddr + ph.memsz; /* no overflow: checked against the window above */

        if (out->segCount > 0) {
            const ElfSegment *prev = &out->segs[out->segCount - 1];
            uint64_t prevEnd = bootAlignUp(prev->vaddr + prev->memsz, ELF_PAGE_SIZE);
            if (ph.vaddr < prevEnd) {
                return BOOT_ERR_ELF_SEGMENT;
            }
        } else {
            out->linkBase = ph.vaddr;
            haveLinkBase = 1;
        }

        out->segs[out->segCount].vaddr = ph.vaddr;
        out->segs[out->segCount].memsz = ph.memsz;
        out->segs[out->segCount].filesz = ph.filesz;
        out->segs[out->segCount].offset = ph.offset;
        out->segs[out->segCount].flags = ph.flags;
        out->segs[out->segCount].pad = 0;
        out->segCount++;
        if (end > maxEnd) {
            maxEnd = end;
        }
    }

    if (!haveLinkBase || out->segCount == 0) {
        return BOOT_ERR_ELF_SEGMENT;
    }
    out->span = bootAlignUp(maxEnd, ELF_PAGE_SIZE) - out->linkBase;

    /* Entry must land inside a PT_LOAD segment that is executable. */
    for (uint32_t i = 0; i < out->segCount; i++) {
        const ElfSegment *s = &out->segs[i];
        if (out->entry >= s->vaddr && out->entry < s->vaddr + s->memsz) {
            if ((s->flags & ELF_PF_X) == 0) {
                return BOOT_ERR_ELF_ENTRY;
            }
            return BOOT_OK;
        }
    }
    return BOOT_ERR_ELF_ENTRY;
}

BootStatus elfLoad(const ElfImage *img, const uint8_t *file, uint8_t *dest) {
    if (img == NULL || file == NULL || dest == NULL) {
        return BOOT_ERR_ELF_HEADER;
    }
    bootMemset(dest, 0, img->span);
    for (uint32_t i = 0; i < img->segCount; i++) {
        const ElfSegment *s = &img->segs[i];
        uint64_t destOff = s->vaddr - img->linkBase;
        /* Belt and suspenders against any other miscomputed span: never copy outside the
         * destination block, even if elfParse's bookkeeping were wrong somehow. */
        if (destOff > img->span || s->filesz > img->span - destOff) {
            return BOOT_ERR_ELF_SEGMENT;
        }
        bootMemcpy(dest + destOff, file + s->offset, s->filesz);
    }
    return BOOT_OK;
}
