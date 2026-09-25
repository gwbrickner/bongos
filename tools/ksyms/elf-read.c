/* See elf-read.h. Raw byte-offset parsing (readLeU16/32/64 at explicit offsets into a whole-file
 * buffer), matching this project's existing host-tool convention (tools/mkfont/main.c) instead of
 * overlaying packed structs on the buffer -- avoids any alignment/strict-aliasing question. */
#define _DEFAULT_SOURCE /* strdup/strnlen (matches tests/host/imgdiff_test.c's convention) */
#include "elf-read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF_SHT_SYMTAB    2u
#define ELF_SHF_EXECINSTR 0x4ULL
#define ELF_STT_FUNC      2u
#define ELF_STB_GLOBAL    1u
#define ELF_STB_WEAK      2u
#define ELF_SHN_UNDEF     0u
#define ELF_SHN_LORESERVE 0xFF00u

static _Noreturn void die(const char *fmt, const char *arg) {
    fprintf(stderr, fmt, arg);
    exit(1);
}

static uint16_t leU16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t leU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t leU64(const uint8_t *p) {
    return (uint64_t)leU32(p) | ((uint64_t)leU32(p + 4) << 32);
}

typedef struct {
    uint32_t nameOff; /* into .shstrtab */
    uint32_t type;
    uint64_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint64_t entsize;
} SectionInfo;

/* Whole file + parsed section header table, shared by every public entry point below so the ELF
 * header/section-header parsing (and its validation) lives in exactly one place. */
typedef struct {
    uint8_t *buf;
    long size;
    SectionInfo *sections;
    uint16_t shnum;
    uint64_t shstrtabOff;
} ElfFile;

static ElfFile elfLoad(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        die("ksyms: %s: cannot open\n", path);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        die("ksyms: %s: seek failed\n", path);
    }
    long size = ftell(f);
    if (size < 64) {
        die("ksyms: %s: too small to be an ELF64 file\n", path);
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        die("ksyms: %s: read failed\n", path);
    }
    fclose(f);

    static const uint8_t elfMagic[4] = {0x7f, 'E', 'L', 'F'};
    if (memcmp(buf, elfMagic, 4) != 0 || buf[4] != 2 /* ELFCLASS64 */ ||
        buf[5] != 1 /* ELFDATA2LSB */) {
        die("ksyms: %s: not a little-endian ELF64 file\n", path);
    }

    uint64_t shoff = leU64(buf + 0x28);
    uint16_t shentsize = leU16(buf + 0x3A);
    uint16_t shnum = leU16(buf + 0x3C);
    uint16_t shstrndx = leU16(buf + 0x3E);
    if (shentsize != 64 || shnum == 0 || shstrndx >= shnum ||
        shoff + (uint64_t)shentsize * shnum > (uint64_t)size) {
        die("ksyms: %s: malformed or missing section header table\n", path);
    }

    SectionInfo *sections = calloc(shnum, sizeof(*sections));
    if (sections == NULL) {
        die("ksyms: out of memory\n", NULL);
    }
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = buf + shoff + (uint64_t)i * shentsize;
        sections[i].nameOff = leU32(sh + 0x00);
        sections[i].type = leU32(sh + 0x04);
        sections[i].flags = leU64(sh + 0x08);
        sections[i].offset = leU64(sh + 0x18);
        sections[i].size = leU64(sh + 0x20);
        sections[i].link = leU32(sh + 0x28);
        sections[i].entsize = leU64(sh + 0x38);
    }

    ElfFile ef = {buf, size, sections, shnum, sections[shstrndx].offset};
    return ef;
}

static void elfFileFree(ElfFile *ef) {
    free(ef->sections);
    free(ef->buf);
}

static int findSymtab(const ElfFile *ef, uint64_t *outOff, uint64_t *outSize, uint64_t *outEnt,
                      uint64_t *outStrtabOff) {
    for (uint16_t i = 0; i < ef->shnum; i++) {
        if (ef->sections[i].type == ELF_SHT_SYMTAB) {
            if (ef->sections[i].link >= ef->shnum) {
                return 0;
            }
            *outOff = ef->sections[i].offset;
            *outSize = ef->sections[i].size;
            *outEnt = ef->sections[i].entsize;
            *outStrtabOff = ef->sections[ef->sections[i].link].offset;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    uint64_t addr;
    uint32_t bindRank;
    char *name;
} RawSym;

static int cmpRaw(const void *a, const void *b) {
    const RawSym *sa = (const RawSym *)a;
    const RawSym *sb = (const RawSym *)b;
    if (sa->addr != sb->addr) {
        return sa->addr < sb->addr ? -1 : 1;
    }
    if (sa->bindRank != sb->bindRank) {
        return sa->bindRank < sb->bindRank ? -1 : 1;
    }
    return strcmp(sa->name, sb->name);
}

ElfFuncSymList elfReadFuncSyms(const char *path) {
    ElfFile ef = elfLoad(path);

    uint64_t symtabOff, symtabSize, symtabEntsize, strtabOff;
    if (!findSymtab(&ef, &symtabOff, &symtabSize, &symtabEntsize, &strtabOff) ||
        symtabEntsize != 24) {
        die("ksyms: %s: no usable SHT_SYMTAB section\n", path);
    }

    size_t symCount = (size_t)(symtabSize / symtabEntsize);
    RawSym *raw = calloc(symCount, sizeof(*raw));
    size_t rawCount = 0;
    for (size_t i = 0; i < symCount; i++) {
        const uint8_t *sym = ef.buf + symtabOff + i * symtabEntsize;
        uint32_t nameOff = leU32(sym + 0x00);
        uint8_t info = sym[0x04];
        uint16_t shndx = leU16(sym + 0x06);
        uint64_t value = leU64(sym + 0x08);
        uint32_t type = info & 0xFu;
        uint32_t bind = info >> 4;

        if (type != ELF_STT_FUNC || shndx == ELF_SHN_UNDEF || shndx >= ELF_SHN_LORESERVE) {
            continue;
        }
        if ((ef.sections[shndx].flags & ELF_SHF_EXECINSTR) == 0) {
            continue;
        }
        const char *name = (const char *)(ef.buf + strtabOff + nameOff);
        size_t nameLen = strnlen(name, 256);
        for (size_t c = 0; c < nameLen; c++) {
            if ((unsigned char)name[c] >= 0x80) {
                die("ksyms: %s: a symbol name has a non-ASCII byte -- refusing to build a "
                    "malformed KSYM blob\n",
                    path);
            }
        }

        raw[rawCount].addr = value;
        raw[rawCount].bindRank = (bind == ELF_STB_GLOBAL) ? 0 : (bind == ELF_STB_WEAK) ? 1 : 2;
        raw[rawCount].name = strdup(name);
        rawCount++;
    }
    elfFileFree(&ef);

    qsort(raw, rawCount, sizeof(*raw), cmpRaw);

    ElfFuncSym *out = calloc(rawCount, sizeof(*out));
    size_t outCount = 0;
    for (size_t i = 0; i < rawCount; i++) {
        if (outCount > 0 && out[outCount - 1].addr == raw[i].addr) {
            free(raw[i].name); /* lower-priority duplicate at the same address */
            continue;
        }
        out[outCount].addr = raw[i].addr;
        out[outCount].name = raw[i].name;
        outCount++;
    }
    free(raw);

    ElfFuncSymList list = {out, outCount};
    return list;
}

void elfFuncSymListFree(ElfFuncSymList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->syms[i].name);
    }
    free(list->syms);
    list->syms = NULL;
    list->count = 0;
}

int elfReadSymbolValue(const char *path, const char *name, uint64_t *outValue) {
    ElfFile ef = elfLoad(path);
    uint64_t symtabOff, symtabSize, symtabEntsize, strtabOff;
    if (!findSymtab(&ef, &symtabOff, &symtabSize, &symtabEntsize, &strtabOff) ||
        symtabEntsize != 24) {
        die("ksyms: %s: no usable SHT_SYMTAB section\n", path);
    }
    size_t symCount = (size_t)(symtabSize / symtabEntsize);
    int found = 0;
    for (size_t i = 0; i < symCount && !found; i++) {
        const uint8_t *sym = ef.buf + symtabOff + i * symtabEntsize;
        uint32_t nameOff = leU32(sym + 0x00);
        const char *symName = (const char *)(ef.buf + strtabOff + nameOff);
        if (strcmp(symName, name) == 0) {
            *outValue = leU64(sym + 0x08);
            found = 1;
        }
    }
    elfFileFree(&ef);
    return found;
}

int elfReadSection(const char *path, const char *sectionName, uint8_t **outData, size_t *outSize) {
    ElfFile ef = elfLoad(path);
    int found = 0;
    for (uint16_t i = 0; i < ef.shnum && !found; i++) {
        const char *name = (const char *)(ef.buf + ef.shstrtabOff + ef.sections[i].nameOff);
        if (strcmp(name, sectionName) == 0) {
            *outSize = (size_t)ef.sections[i].size;
            *outData = malloc(*outSize > 0 ? *outSize : 1);
            memcpy(*outData, ef.buf + ef.sections[i].offset, *outSize);
            found = 1;
        }
    }
    elfFileFree(&ef);
    return found;
}
