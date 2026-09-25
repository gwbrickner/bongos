/* tools/mksyms: builds the embedded kernel symbol table (docs/specs/ksyms.md, D-073) from
 * kernel.elf's own ELF64 symtab. Host tool (own from-scratch ELF64 reader -- ARCHITECTURE §0's
 * host-tool exception still keeps parsers self-written rather than reaching for a third-party
 * lib, matching tools/mkimage's own GPT/ELF conventions).
 *
 * Usage:
 *   mksyms -o <out.bin> <kernel.elf>   -- writes the ksyms blob derived from <kernel.elf>
 *   mksyms --check <kernel.elf>        -- re-derives the same blob from <kernel.elf>'s own
 *                                         symtab and byte-compares it against that same file's
 *                                         already-embedded .ksyms section; nonzero exit on any
 *                                         mismatch (mk/kernel.mk's two-pass link runs this after
 *                                         the real blob is linked in, as a build-time proof that
 *                                         embedding it didn't shift any .text symbol).
 *
 * Kept as one file: the parser, the dedup/encode logic, and argv handling are all small and
 * tightly coupled (the same read-LE-field helpers serve both the input ELF and the ksyms format
 * itself), so splitting them across files would add indirection without adding clarity.
 */
#include "ksyms-format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer {
    uint8_t *data;
    size_t len;
} Buffer;

static Buffer readFile(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "mksyms: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fprintf(stderr, "mksyms: cannot size %s\n", path);
        exit(1);
    }
    uint8_t *data = malloc((size_t)size);
    if (data == NULL || (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size)) {
        fprintf(stderr, "mksyms: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    Buffer b = {data, (size_t)size};
    return b;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* --- Minimal ELF64 reader: just enough to find .text/.symtab/.strtab/.ksyms and walk Elf64_Sym
 * entries by hand (fixed byte offsets, matching the ELF64 spec exactly). */

typedef struct Section {
    char name[64];
    uint32_t type;
    uint64_t addr, offset, size;
    uint32_t link, entsize;
} Section;

typedef struct ElfFile {
    Buffer buf;
    Section *sections;
    uint32_t sectionCount;
} ElfFile;

static void elfFail(const char *why) {
    fprintf(stderr, "mksyms: malformed ELF: %s\n", why);
    exit(1);
}

static ElfFile elfOpen(const char *path) {
    ElfFile e;
    e.buf = readFile(path);
    const uint8_t *d = e.buf.data;
    if (e.buf.len < 64 || d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') {
        elfFail("not an ELF file");
    }
    if (d[4] != 2 || d[5] != 1) {
        elfFail("not ELF64 little-endian");
    }
    uint64_t shoff = le64(d + 0x28);
    uint16_t shentsize = le16(d + 0x3A);
    uint16_t shnum = le16(d + 0x3C);
    uint16_t shstrndx = le16(d + 0x3E);
    if (shentsize < 64 || (uint64_t)shnum * shentsize + shoff > e.buf.len) {
        elfFail("bad section header table");
    }
    if (shstrndx >= shnum) {
        elfFail("bad shstrndx");
    }

    const uint8_t *shstrHdr = d + shoff + (uint64_t)shstrndx * shentsize;
    uint64_t shstrOff = le64(shstrHdr + 0x18);
    uint64_t shstrSize = le64(shstrHdr + 0x20);
    if (shstrOff + shstrSize > e.buf.len) {
        elfFail("bad section name string table");
    }
    const char *shstr = (const char *)(d + shstrOff);

    e.sectionCount = shnum;
    e.sections = calloc(shnum, sizeof(Section));
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + (uint64_t)i * shentsize;
        uint32_t nameOff = le32(sh + 0x00);
        if (nameOff >= shstrSize) {
            elfFail("section name out of range");
        }
        size_t maxLen = shstrSize - nameOff;
        snprintf(e.sections[i].name, sizeof(e.sections[i].name), "%.*s", (int)maxLen,
                 shstr + nameOff);
        e.sections[i].type = le32(sh + 0x04);
        e.sections[i].addr = le64(sh + 0x10);
        e.sections[i].offset = le64(sh + 0x18);
        e.sections[i].size = le64(sh + 0x20);
        e.sections[i].link = le32(sh + 0x28);
        e.sections[i].entsize = le64(sh + 0x38);
    }
    return e;
}

static const Section *elfFindSection(const ElfFile *e, const char *name) {
    for (uint32_t i = 0; i < e->sectionCount; i++) {
        if (strcmp(e->sections[i].name, name) == 0) {
            return &e->sections[i];
        }
    }
    return NULL;
}

/* --- Symbol extraction and dedup. */

typedef struct RawSym {
    char name[256];
    uint64_t offset; /* relative to .text's sh_addr */
    uint64_t size;   /* 0 means "derive from the next symbol" */
    int bindRank;    /* 0=GLOBAL, 1=WEAK, 2=LOCAL (lower is preferred) */
    int isFunc;      /* 1 = STT_FUNC, 0 = STT_NOTYPE */
} RawSym;

static int bindRankOf(uint8_t info) {
    uint8_t bind = info >> 4;
    if (bind == 1) {
        return 0; /* GLOBAL */
    }
    if (bind == 2) {
        return 1; /* WEAK */
    }
    return 2; /* LOCAL or anything else */
}

static int symBetter(const RawSym *a, const RawSym *b) {
    /* Returns 1 if `a` should win over `b` for the same offset. */
    if (a->isFunc != b->isFunc) {
        return a->isFunc > b->isFunc;
    }
    if (a->bindRank != b->bindRank) {
        return a->bindRank < b->bindRank;
    }
    size_t la = strlen(a->name), lb = strlen(b->name);
    if (la != lb) {
        return la < lb;
    }
    return strcmp(a->name, b->name) < 0;
}

static int rawSymCmp(const void *pa, const void *pb) {
    const RawSym *a = pa, *b = pb;
    if (a->offset != b->offset) {
        return a->offset < b->offset ? -1 : 1;
    }
    /* Stable-ish tiebreak so qsort's ordering among same-offset entries is deterministic before
     * dedup picks the actual winner. */
    return strcmp(a->name, b->name);
}

static RawSym *extractSymbols(const ElfFile *e, uint64_t *outCount, uint64_t textAddr,
                              uint64_t textSize) {
    const Section *symtab = elfFindSection(e, ".symtab");
    if (symtab == NULL) {
        elfFail("no .symtab (link with symbols kept, no --strip)");
    }
    const Section *strtab = &e->sections[symtab->link];
    const char *strs = (const char *)(e->buf.data + strtab->offset);
    uint64_t strsSize = strtab->size;

    uint32_t textIndex = 0;
    int haveTextIndex = 0;
    for (uint32_t i = 0; i < e->sectionCount; i++) {
        if (e->sections[i].addr == textAddr && strcmp(e->sections[i].name, ".text") == 0) {
            textIndex = i;
            haveTextIndex = 1;
            break;
        }
    }
    if (!haveTextIndex) {
        elfFail("no .text section");
    }

    uint32_t entsize = symtab->entsize ? symtab->entsize : 24;
    uint64_t rawCount = symtab->size / entsize;
    RawSym *syms = calloc(rawCount, sizeof(RawSym));
    uint64_t n = 0;

    for (uint64_t i = 0; i < rawCount; i++) {
        const uint8_t *s = e->buf.data + symtab->offset + i * entsize;
        uint32_t nameOff = le32(s + 0x00);
        uint8_t info = s[0x04];
        uint16_t shndx = le16(s + 0x06);
        uint64_t value = le64(s + 0x08);
        uint64_t size = le64(s + 0x10);
        uint8_t type = info & 0xF;

        if (shndx != textIndex) {
            continue;
        }
        if (type != 2 /* FUNC */ && type != 0 /* NOTYPE */) {
            continue;
        }
        if (nameOff >= strsSize) {
            elfFail("symbol name out of range");
        }
        const char *name = strs + nameOff;
        if (name[0] == '\0' || (name[0] == '.' && name[1] == 'L')) {
            continue;
        }
        if (value < textAddr || value >= textAddr + textSize) {
            continue;
        }
        size_t nameLen = strlen(name);
        if (nameLen > 255) {
            fprintf(stderr, "mksyms: symbol name too long (>255): %s\n", name);
            exit(1);
        }

        RawSym *out = &syms[n++];
        memset(out->name, 0, sizeof(out->name));
        memcpy(out->name, name, nameLen);
        out->offset = value - textAddr;
        out->size = size;
        out->bindRank = bindRankOf(info);
        out->isFunc = (type == 2);
    }

    qsort(syms, n, sizeof(RawSym), rawSymCmp);

    /* Dedup by offset, keeping the best-ranked entry per group. */
    RawSym *deduped = calloc(n, sizeof(RawSym));
    uint64_t outN = 0;
    uint64_t i = 0;
    while (i < n) {
        uint64_t j = i;
        RawSym best = syms[i];
        while (j < n && syms[j].offset == syms[i].offset) {
            if (symBetter(&syms[j], &best)) {
                best = syms[j];
            }
            j++;
        }
        deduped[outN++] = best;
        i = j;
    }
    free(syms);

    /* Fill in zero sizes from the gap to the next entry (or to the end of .text). */
    for (uint64_t k = 0; k < outN; k++) {
        if (deduped[k].size != 0) {
            continue;
        }
        uint64_t end = (k + 1 < outN) ? deduped[k + 1].offset : textSize;
        deduped[k].size = end - deduped[k].offset;
    }

    *outCount = outN;
    return deduped;
}

/* --- Encoding (front-coded names with restart points, ARCHITECTURE ksyms v1). */

#define RESTART_INTERVAL 16

typedef struct ByteBuf {
    uint8_t *data;
    size_t len, cap;
} ByteBuf;

static void bbPush(ByteBuf *b, uint8_t v) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->data = realloc(b->data, b->cap);
    }
    b->data[b->len++] = v;
}

static void bbPushU32(ByteBuf *b, uint32_t v) {
    bbPush(b, (uint8_t)(v & 0xFF));
    bbPush(b, (uint8_t)((v >> 8) & 0xFF));
    bbPush(b, (uint8_t)((v >> 16) & 0xFF));
    bbPush(b, (uint8_t)((v >> 24) & 0xFF));
}

static void bbPushU16(ByteBuf *b, uint16_t v) {
    bbPush(b, (uint8_t)(v & 0xFF));
    bbPush(b, (uint8_t)((v >> 8) & 0xFF));
}

static Buffer encode(const RawSym *syms, uint64_t count) {
    ByteBuf addrs = {0}, restarts = {0}, names = {0};

    char prev[256];
    prev[0] = '\0';
    for (uint64_t i = 0; i < count; i++) {
        bbPushU32(&addrs, (uint32_t)syms[i].offset);
        bbPushU32(&addrs, (uint32_t)syms[i].size);

        size_t nameLen = strlen(syms[i].name);
        if (i % RESTART_INTERVAL == 0) {
            bbPushU32(&restarts, (uint32_t)names.len);
            bbPush(&names, 0);
            bbPush(&names, (uint8_t)nameLen);
            for (size_t k = 0; k < nameLen; k++) {
                bbPush(&names, (uint8_t)syms[i].name[k]);
            }
        } else {
            size_t prevLen = strlen(prev);
            size_t shared = 0;
            size_t maxShared = prevLen < nameLen ? prevLen : nameLen;
            while (shared < maxShared && prev[shared] == syms[i].name[shared]) {
                shared++;
            }
            if (shared > 255) {
                shared = 255;
            }
            size_t suffixLen = nameLen - shared;
            bbPush(&names, (uint8_t)shared);
            bbPush(&names, (uint8_t)suffixLen);
            for (size_t k = 0; k < suffixLen; k++) {
                bbPush(&names, (uint8_t)syms[i].name[shared + k]);
            }
        }
        memcpy(prev, syms[i].name, nameLen + 1);
    }
    while (names.len % 8 != 0) {
        bbPush(&names, 0);
    }

    uint32_t addrsOff = KSYMS_HEADER_SIZE;
    uint32_t restartsOff = (uint32_t)(addrsOff + addrs.len);
    uint32_t namesOff = (uint32_t)(restartsOff + restarts.len);

    ByteBuf out = {0};
    bbPushU32(&out, KSYMS_MAGIC);
    bbPushU16(&out, (uint16_t)KSYMS_VERSION);
    bbPushU16(&out, (uint16_t)KSYMS_HEADER_SIZE);
    bbPushU32(&out, (uint32_t)count);
    bbPushU32(&out, RESTART_INTERVAL);
    bbPushU32(&out, addrsOff);
    bbPushU32(&out, restartsOff);
    bbPushU32(&out, namesOff);
    bbPushU32(&out, (uint32_t)names.len);
    for (size_t k = 0; k < addrs.len; k++) {
        bbPush(&out, addrs.data[k]);
    }
    for (size_t k = 0; k < restarts.len; k++) {
        bbPush(&out, restarts.data[k]);
    }
    for (size_t k = 0; k < names.len; k++) {
        bbPush(&out, names.data[k]);
    }

    free(addrs.data);
    free(restarts.data);
    free(names.data);

    Buffer result = {out.data, out.len};
    return result;
}

static Buffer buildKsyms(const char *elfPath) {
    ElfFile e = elfOpen(elfPath);
    const Section *text = elfFindSection(&e, ".text");
    if (text == NULL) {
        elfFail("no .text section");
    }
    uint64_t count = 0;
    RawSym *syms = extractSymbols(&e, &count, text->addr, text->size);
    Buffer blob = encode(syms, count);
    free(syms);
    free(e.sections);
    free(e.buf.data);
    return blob;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "-o") == 0) {
        Buffer blob = buildKsyms(argv[3]);
        FILE *out = fopen(argv[2], "wb");
        if (out == NULL || fwrite(blob.data, 1, blob.len, out) != blob.len) {
            fprintf(stderr, "mksyms: cannot write %s\n", argv[2]);
            return 1;
        }
        fclose(out);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--check") == 0) {
        Buffer expected = buildKsyms(argv[2]);
        ElfFile e = elfOpen(argv[2]);
        const Section *ksyms = elfFindSection(&e, ".ksyms");
        if (ksyms == NULL) {
            elfFail("no .ksyms section to check against");
        }
        if (ksyms->size != expected.len ||
            memcmp(e.buf.data + ksyms->offset, expected.data, expected.len) != 0) {
            fprintf(stderr,
                    "mksyms: --check FAILED: the embedded .ksyms section (%llu bytes) doesn't "
                    "match the table re-derived from %s's own symtab (%llu bytes)\n",
                    (unsigned long long)ksyms->size, argv[2], (unsigned long long)expected.len);
            return 1;
        }
        return 0;
    }
    fprintf(stderr, "usage: mksyms -o <out.bin> <kernel.elf>\n"
                    "       mksyms --check <kernel.elf>\n");
    return 1;
}
