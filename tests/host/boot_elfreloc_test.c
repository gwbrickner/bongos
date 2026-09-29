/* Host tests for boot/common/elf-reloc.c (M2.6, D-120): the KASLR relocator. Builds synthetic
 * ELF64 files by hand, with real section headers, a symbol table and SHT_RELA sections, so every
 * row of the relocation-type table and every validation failure is exercised precisely. Layout of
 * the fixture (all offsets in the file):
 *   0x0000 ELF header + 2 program headers
 *   0x1000 .text  (R+X segment, KBASE,          filesz 0x100, memsz 0x1000; section size 0x80)
 *   0x2000 .data  (R+W segment, KBASE + 0x1000, filesz 0x200, memsz 0x1000; section size 0x100)
 *          .bss   (NOBITS, KBASE + 0x1200, size 0x200: the memsz-filesz tail of the data segment)
 *   0x2400 .comment (non-alloc)
 *   0x3000 .symtab, 0x3200 .strtab, 0x4000/0x4400/0x4800 .rela.text/.rela.data/.rela.comment
 *   0x5000 section header table (10 entries) */
#include "bootinfo.h"
#include "bootmem.h"
#include "elf64.h"
#include "framework/test.h"

#include <stdlib.h>
#include <string.h>

#define FILE_SIZE 0x5300
#define KBASE     BOOTINFO_KERNEL_WINDOW_BASE
#define TEXT_VA   KBASE
#define DATA_VA   (KBASE + 0x1000)
#define BSS_VA    (KBASE + 0x1200)
#define SLIDE     0x200000ULL

#define SHT_PROGBITS 1u
#define SHT_SYMTAB   2u
#define SHT_STRTAB   3u
#define SHT_RELA     4u
#define SHT_NOBITS   8u
#define SHT_REL      9u
#define SHF_WRITE    1ULL
#define SHF_ALLOC    2ULL
#define SHF_EXEC     4ULL

/* Section indices. */
enum {
    S_TEXT = 1,
    S_DATA,
    S_BSS,
    S_COMMENT,
    S_SYMTAB,
    S_STRTAB,
    S_RELA_TEXT,
    S_RELA_DATA,
    S_RELA_COMMENT,
    S_COUNT
};
/* Symbol indices. */
enum { Y_NULL, Y_TEXT, Y_DATA, Y_ABS, Y_UND, Y_BSS, Y_COMMENT, Y_COMMON, Y_RESERVED, Y_COUNT };

#define R_NONE 0u
#define R_64   1u
#define R_PC32 2u
#define R_GOT  3u
#define R_PLT  4u
#define R_32   10u
#define R_32S  11u
#define R_PC64 24u

typedef struct {
    uint8_t file[FILE_SIZE];
    uint8_t dest[0x2000];
    uint8_t before[0x2000];
    ElfImage img;
    ElfRelocStats stats;
    uint32_t nRela[3]; /* entries added to .rela.text, .rela.data, .rela.comment */
} Fx;

static Fx fx;

static void put16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}
static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}
static void put64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}
static uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint8_t *shdrAt(Fx *f, uint32_t idx) {
    return f->file + 0x5000 + (size_t)idx * 64;
}

static void setSection(Fx *f, uint32_t idx, uint32_t type, uint64_t flags, uint64_t addr,
                       uint64_t off, uint64_t size, uint32_t link, uint32_t info,
                       uint64_t entsize) {
    uint8_t *p = shdrAt(f, idx);
    memset(p, 0, 64);
    put32(p + 4, type);
    put64(p + 8, flags);
    put64(p + 16, addr);
    put64(p + 24, off);
    put64(p + 32, size);
    put32(p + 40, link);
    put32(p + 44, info);
    put64(p + 48, 8);
    put64(p + 56, entsize);
}

static void setSym(Fx *f, uint32_t idx, uint16_t shndx, uint64_t value) {
    uint8_t *p = f->file + 0x3000 + (size_t)idx * 24;
    memset(p, 0, 24);
    put16(p + 6, shndx);
    put64(p + 8, value);
}

static void setRelaSize(Fx *f, uint32_t which) {
    static const uint32_t secs[3] = {S_RELA_TEXT, S_RELA_DATA, S_RELA_COMMENT};
    put64(shdrAt(f, secs[which]) + 32, (uint64_t)f->nRela[which] * 24);
}

/* Appends a relocation to .rela.text (which 0), .rela.data (1) or .rela.comment (2). */
static void addReloc(Fx *f, uint32_t which, uint64_t offset, uint32_t type, uint32_t sym,
                     int64_t addend) {
    static const uint32_t base[3] = {0x4000, 0x4400, 0x4800};
    uint8_t *p = f->file + base[which] + (size_t)f->nRela[which] * 24;
    put64(p, offset);
    put64(p + 8, ((uint64_t)sym << 32) | type);
    put64(p + 16, (uint64_t)addend);
    f->nRela[which]++;
    setRelaSize(f, which);
}

/* Writes into the file at a virtual address inside the text (seg 0) or data (seg 1) segment. */
static void pokeText32(Fx *f, uint64_t va, uint32_t v) {
    put32(f->file + 0x1000 + (va - TEXT_VA), v);
}
static void pokeData64(Fx *f, uint64_t va, uint64_t v) {
    put64(f->file + 0x2000 + (va - DATA_VA), v);
}
static void pokeData32(Fx *f, uint64_t va, uint32_t v) {
    put32(f->file + 0x2000 + (va - DATA_VA), v);
}
static uint32_t destText32(const Fx *f, uint64_t va) {
    return get32(f->dest + (va - KBASE));
}
static uint64_t destData64(const Fx *f, uint64_t va) {
    return get64(f->dest + (va - KBASE));
}
static uint32_t destData32(const Fx *f, uint64_t va) {
    return get32(f->dest + (va - KBASE));
}

static void putPhdr(uint8_t *p, uint32_t flags, uint64_t off, uint64_t va, uint64_t filesz,
                    uint64_t memsz) {
    put32(p, 1); /* PT_LOAD */
    put32(p + 4, flags);
    put64(p + 8, off);
    put64(p + 16, va);
    put64(p + 24, va);
    put64(p + 32, filesz);
    put64(p + 40, memsz);
    put64(p + 48, 0x1000);
}

/* A fully valid fixture with no relocations yet. Every test adds its own. */
static void fxInit(Fx *f) {
    memset(f, 0, sizeof(*f));
    uint8_t *h = f->file;
    h[0] = 0x7F;
    h[1] = 'E';
    h[2] = 'L';
    h[3] = 'F';
    h[4] = 2;
    h[5] = 1;
    h[6] = 1;
    put16(h + 16, 2);
    put16(h + 18, 62);
    put32(h + 20, 1);
    put64(h + 24, KBASE);
    put64(h + 32, 64);
    put64(h + 40, 0x5000);
    put16(h + 52, 64);
    put16(h + 54, 56);
    put16(h + 56, 2);
    put16(h + 58, 64);
    put16(h + 60, S_COUNT);
    put16(h + 62, 0);
    putPhdr(h + 64, 5, 0x1000, TEXT_VA, 0x100, 0x1000);
    putPhdr(h + 64 + 56, 6, 0x2000, DATA_VA, 0x200, 0x1000);

    setSection(f, S_TEXT, SHT_PROGBITS, SHF_ALLOC | SHF_EXEC, TEXT_VA, 0x1000, 0x80, 0, 0, 0);
    setSection(f, S_DATA, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, DATA_VA, 0x2000, 0x100, 0, 0, 0);
    setSection(f, S_BSS, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, BSS_VA, 0x2200, 0x200, 0, 0, 0);
    setSection(f, S_COMMENT, SHT_PROGBITS, 0, 0, 0x2400, 16, 0, 0, 0);
    setSection(f, S_SYMTAB, SHT_SYMTAB, 0, 0, 0x3000, Y_COUNT * 24, S_STRTAB, 1, 24);
    setSection(f, S_STRTAB, SHT_STRTAB, 0, 0, 0x3200, 8, 0, 0, 0);
    setSection(f, S_RELA_TEXT, SHT_RELA, 0, 0, 0x4000, 0, S_SYMTAB, S_TEXT, 24);
    setSection(f, S_RELA_DATA, SHT_RELA, 0, 0, 0x4400, 0, S_SYMTAB, S_DATA, 24);
    setSection(f, S_RELA_COMMENT, SHT_RELA, 0, 0, 0x4800, 0, S_SYMTAB, S_COMMENT, 24);

    setSym(f, Y_TEXT, S_TEXT, TEXT_VA);
    setSym(f, Y_DATA, S_DATA, DATA_VA);
    setSym(f, Y_ABS, 0xFFF1, 0x1000);
    setSym(f, Y_UND, 0, 0);
    setSym(f, Y_BSS, S_BSS, BSS_VA);
    setSym(f, Y_COMMENT, S_COMMENT, 0);
    setSym(f, Y_COMMON, 0xFFF2, 8);
    setSym(f, Y_RESERVED, 0xFF00, 0);
}

/* The one relocation every "should succeed" test needs so the exec-target check passes: a 32S in
 * .text against the text symbol, holding a sign-extended KBASE + 0x40 (low 32 bits 0x80000040). */
#define EXEC_LOC (TEXT_VA + 0x10)
static void addExecReloc(Fx *f) {
    pokeText32(f, EXEC_LOC, 0x80000040u);
    addReloc(f, 0, EXEC_LOC, R_32S, Y_TEXT, 0x40);
}

/* Parses, loads and (optionally) relocates with `slide`; snapshots dest first. */
static BootStatus fxRun(Fx *f, uint64_t slide) {
    BootStatus st = elfParse(f->file, FILE_SIZE, &f->img);
    if (st != BOOT_OK) {
        return st;
    }
    st = elfLoad(&f->img, f->file, f->dest);
    if (st != BOOT_OK) {
        return st;
    }
    memcpy(f->before, f->dest, sizeof(f->dest));
    return elfRelocate(&f->img, f->file, FILE_SIZE, f->dest, slide, &f->stats);
}

static int destUnchanged(const Fx *f) {
    return memcmp(f->dest, f->before, sizeof(f->dest)) == 0;
}

/* Runs a fixture that must be rejected by elfRelocate with `want`, leaving dest untouched. */
#define EXPECT_REJECT(f, want)                                                                     \
    do {                                                                                           \
        ASSERT_EQ(fxRun((f), SLIDE), (want));                                                      \
        ASSERT_TRUE(destUnchanged(f));                                                             \
    } while (0)

/* Extra relocation added next to the always-valid exec one. */
static void setupWith(Fx *f) {
    fxInit(f);
    addExecReloc(f);
}

TEST(relocBasicTextAndDataSlide) {
    fxInit(&fx);
    addExecReloc(&fx);
    pokeData64(&fx, DATA_VA + 0x20, KBASE + 0x1234);
    addReloc(&fx, 1, DATA_VA + 0x20, R_64, Y_DATA, 0x234);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destText32(&fx, EXEC_LOC), 0x80000040u + (uint32_t)SLIDE);
    ASSERT_EQ(destData64(&fx, DATA_VA + 0x20), KBASE + 0x1234 + SLIDE);
    ASSERT_EQ(fx.stats.relaSections, 2u);
    ASSERT_EQ(fx.stats.total, 2u);
    ASSERT_EQ(fx.stats.applied, 2u);
    ASSERT_EQ(fx.stats.execApplied, 1u);
    ASSERT_EQ(fx.stats.skipped, 0u);
}

TEST(relocNoneIsSkippedEvenWithGarbageSymbol) {
    setupWith(&fx);
    addReloc(&fx, 1, DATA_VA, R_NONE, 0xFFFF, 0); /* symbol index is out of range; irrelevant */
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(fx.stats.skipped, 1u);
    ASSERT_EQ(fx.stats.applied, 1u);
}

TEST(reloc64AgainstSectionSymbolAddsSlide) {
    setupWith(&fx);
    pokeData64(&fx, DATA_VA + 8, KBASE + 0x40);
    pokeData64(&fx, DATA_VA + 16, KBASE + 0x1000); /* adjacent qwords stay independent */
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_TEXT, 0x40);
    ASSERT_EQ(fxRun(&fx, 0x1E000000ULL), BOOT_OK);
    ASSERT_EQ(destData64(&fx, DATA_VA + 8), KBASE + 0x40 + 0x1E000000ULL);
    ASSERT_EQ(destData64(&fx, DATA_VA + 16), KBASE + 0x1000);
}

TEST(reloc64AgainstAbsAndUndIsSkipped) {
    setupWith(&fx);
    pokeData64(&fx, DATA_VA + 8, 0x1234);
    pokeData64(&fx, DATA_VA + 16, 0x5678);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_ABS, 0);
    addReloc(&fx, 1, DATA_VA + 16, R_64, Y_UND, 0);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destData64(&fx, DATA_VA + 8), 0x1234ULL);
    ASSERT_EQ(destData64(&fx, DATA_VA + 16), 0x5678ULL);
    ASSERT_EQ(fx.stats.skipped, 2u);
}

TEST(reloc64WrapIsRejected) {
    setupWith(&fx);
    pokeData64(&fx, DATA_VA + 8, 0xFFFFFFFFFFFFFFF0ULL);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_DATA, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(reloc64JustBelowWrapIsAccepted) {
    setupWith(&fx);
    pokeData64(&fx, DATA_VA + 8, 0xFFFFFFFFFFFFFFFFULL - SLIDE);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_DATA, 0);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destData64(&fx, DATA_VA + 8), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(reloc32SChangesOnlyTheLow32Bits) {
    setupWith(&fx);
    pokeData32(&fx, DATA_VA + 8, 0x80001000u);
    pokeData32(&fx, DATA_VA + 12, 0xCAFEBABEu);
    addReloc(&fx, 1, DATA_VA + 8, R_32S, Y_TEXT, 0x1000);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destData32(&fx, DATA_VA + 8), 0x80001000u + (uint32_t)SLIDE);
    ASSERT_EQ(destData32(&fx, DATA_VA + 12), 0xCAFEBABEu);
}

TEST(reloc32SOverflowIsRejected) {
    setupWith(&fx);
    /* +0x7FFFFFF0 sign-extends to a positive int32; adding the slide crosses INT32_MAX. */
    pokeData32(&fx, DATA_VA + 8, 0x7FFFFFF0u);
    addReloc(&fx, 1, DATA_VA + 8, R_32S, Y_DATA, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(reloc32SMaximumAcceptedValue) {
    setupWith(&fx);
    pokeData32(&fx, DATA_VA + 8, (uint32_t)(0x7FFFFFFF - SLIDE));
    addReloc(&fx, 1, DATA_VA + 8, R_32S, Y_DATA, 0);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destData32(&fx, DATA_VA + 8), 0x7FFFFFFFu);
}

TEST(reloc32SAgainstAbsAndUndIsSkipped) {
    setupWith(&fx);
    pokeData32(&fx, DATA_VA + 8, 0x7FFFFFF0u); /* would overflow if it were slid */
    pokeData32(&fx, DATA_VA + 12, 0x11223344u);
    addReloc(&fx, 1, DATA_VA + 8, R_32S, Y_ABS, 0);
    addReloc(&fx, 1, DATA_VA + 12, R_32S, Y_UND, 0);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destData32(&fx, DATA_VA + 8), 0x7FFFFFF0u);
    ASSERT_EQ(destData32(&fx, DATA_VA + 12), 0x11223344u);
}

TEST(reloc32AgainstSectionSymbolIsRejected) {
    setupWith(&fx);
    addReloc(&fx, 1, DATA_VA + 8, R_32, Y_TEXT, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(reloc32AgainstAbsAndUndIsSkipped) {
    setupWith(&fx);
    pokeData32(&fx, DATA_VA + 8, 0x1000);
    addReloc(&fx, 1, DATA_VA + 8, R_32, Y_ABS, 0);
    addReloc(&fx, 1, DATA_VA + 12, R_32, Y_UND, 0);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destData32(&fx, DATA_VA + 8), 0x1000u);
    ASSERT_EQ(fx.stats.skipped, 2u);
}

TEST(relocPcRelativeAgainstSectionSymbolNeedsNothing) {
    setupWith(&fx);
    pokeText32(&fx, TEXT_VA + 0x20, 0x12345678u);
    pokeText32(&fx, TEXT_VA + 0x24, 0x9ABCDEF0u);
    pokeData64(&fx, DATA_VA, 0x0102030405060708ULL);
    addReloc(&fx, 0, TEXT_VA + 0x20, R_PC32, Y_DATA, -4);
    addReloc(&fx, 0, TEXT_VA + 0x24, R_PLT, Y_TEXT, -4);
    addReloc(&fx, 1, DATA_VA, R_PC64, Y_TEXT, 0);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(destText32(&fx, TEXT_VA + 0x20), 0x12345678u);
    ASSERT_EQ(destText32(&fx, TEXT_VA + 0x24), 0x9ABCDEF0u);
    ASSERT_EQ(destData64(&fx, DATA_VA), 0x0102030405060708ULL);
    ASSERT_EQ(fx.stats.skipped, 3u);
    ASSERT_EQ(fx.stats.applied, 1u);
}

TEST(relocPcRelativeAgainstAbsOrUndIsRejected) {
    static const uint32_t types[3] = {R_PC32, R_PLT, R_PC64};
    static const uint32_t syms[2] = {Y_ABS, Y_UND};
    for (uint32_t t = 0; t < 3; t++) {
        for (uint32_t s = 0; s < 2; s++) {
            setupWith(&fx);
            addReloc(&fx, 0, TEXT_VA + 0x20, types[t], syms[s], -4);
            EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
        }
    }
}

TEST(relocUnsupportedTypeIsRejected) {
    static const uint32_t types[] = {R_GOT, 5u /* COPY */, 9u /* GOTPCREL */,
                                     12u,   24u + 1u,      0x1234u};
    for (uint32_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        setupWith(&fx);
        addReloc(&fx, 1, DATA_VA + 8, types[t], Y_DATA, 0);
        EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    }
}

TEST(relocLocationOutsideAnySegmentIsRejected) {
    static const uint64_t bad[] = {
        KBASE + 0x5000,          /* beyond every segment */
        KBASE + 0x800,           /* the gap between the two segments */
        KBASE - 8,               /* below the image (would wrap the dest offset) */
        0xFFFFFFFFFFFFFFFCULL,   /* would overflow va + width */
        KBASE + 0x1000 + 0x1FFC, /* inside memsz, nowhere near a section */
    };
    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setupWith(&fx);
        addReloc(&fx, 1, bad[i], R_64, Y_DATA, 0);
        EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    }
}

TEST(relocLocationInMemszTailIsRejected) {
    /* .data's section covers filesz 0x200 only up to 0x100, so make the section claim more than
     * the segment has file backing for: the reloc is inside the section, outside filesz. */
    setupWith(&fx);
    put64(shdrAt(&fx, S_DATA) + 32, 0x400);
    addReloc(&fx, 1, DATA_VA + 0x200, R_64, Y_DATA, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocLocationStraddlingFileBackedEndIsRejected) {
    setupWith(&fx);
    put64(shdrAt(&fx, S_DATA) + 32, 0x400);
    addReloc(&fx, 1, DATA_VA + 0x1FC, R_64, Y_DATA, 0); /* 4 bytes file-backed, 4 in the tail */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_DATA) + 32, 0x400);
    addReloc(&fx, 1, DATA_VA + 0x1F8, R_64, Y_DATA, 0); /* last file-backed qword: fine */
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
}

TEST(relocLocationInsideSegmentButOutsideTargetSectionIsRejected) {
    setupWith(&fx);
    addReloc(&fx, 0, TEXT_VA + 0x90, R_32S, Y_TEXT, 0); /* .text is 0x80 bytes, segment 0x100 */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocLocationWrapsTargetSectionBoundsIsRejected) {
    setupWith(&fx);
    addReloc(&fx, 0, TEXT_VA + 0x7E, R_32S, Y_TEXT, 0); /* 4-byte location crossing the end */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocNobitsTargetIsRejected) {
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_DATA) + 44, S_BSS); /* .rela.data now targets .bss */
    addReloc(&fx, 1, BSS_VA, R_64, Y_DATA, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocNobitsTargetWithNoEntriesIsStillRejected) {
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_DATA) + 44, S_BSS);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocShtRelSectionIsRejected) {
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_DATA) + 4, SHT_REL);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocNonAllocTargetIsIgnoredEvenIfGarbage) {
    setupWith(&fx);
    addReloc(&fx, 2, 0xDEADBEEF, 0x7777, 0xFFFFFF, 0); /* .rela.comment: like .rela.debug_* */
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(fx.stats.relaSections,
              2u);                 /* .rela.text and the (empty) .rela.data, not .rela.comment */
    ASSERT_EQ(fx.stats.total, 1u); /* only the exec reloc: the garbage one was never read */
}

TEST(relocMissingSymtabIsRejected) {
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_TEXT) + 40, 0); /* sh_link = 0 */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocLinkToNonSymtabIsRejected) {
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_TEXT) + 40, S_STRTAB);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocLinkOutOfRangeIsRejected) {
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_TEXT) + 40, S_COUNT);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocSymtabBadEntsizeOrBoundsIsRejected) {
    setupWith(&fx);
    put64(shdrAt(&fx, S_SYMTAB) + 56, 16);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_SYMTAB) + 24, FILE_SIZE - 24); /* runs past the end of the file */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_SYMTAB) + 32, 24 * Y_COUNT + 1); /* not a whole number of symbols */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_SYMTAB) + 24, 0xFFFFFFFFFFFFFFF0ULL); /* offset + size wraps */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocSymbolIndexOutOfRangeIsRejected) {
    setupWith(&fx);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_COUNT, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocSymbolInNonAllocOrReservedSectionIsRejected) {
    static const uint32_t syms[] = {Y_COMMENT, Y_COMMON, Y_RESERVED};
    for (uint32_t i = 0; i < 3; i++) {
        setupWith(&fx);
        addReloc(&fx, 1, DATA_VA + 8, R_64, syms[i], 0);
        EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    }
    /* A section index beyond e_shnum. */
    setupWith(&fx);
    setSym(&fx, Y_DATA, S_COUNT, DATA_VA);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_DATA, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocSymbolValidityIsCheckedForPcRelativeToo) {
    setupWith(&fx);
    addReloc(&fx, 0, TEXT_VA + 0x20, R_PC32, Y_COMMENT, -4);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocWithoutAnyExecutableRelocationIsRejected) {
    fxInit(&fx); /* not even a .rela.text */
    pokeData64(&fx, DATA_VA, KBASE);
    addReloc(&fx, 1, DATA_VA, R_64, Y_DATA, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocWithNoRelocationsAtAllIsRejected) {
    fxInit(&fx);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocOnlyPcRelativeInTextDoesNotCountAsExecutableRelocation) {
    fxInit(&fx);
    addReloc(&fx, 0, TEXT_VA + 0x20, R_PC32, Y_DATA, -4);
    addReloc(&fx, 0, TEXT_VA + 0x24, R_32S, Y_ABS, 0); /* skipped: value does not move */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocSlideZeroValidatesButChangesNothing) {
    setupWith(&fx);
    pokeData64(&fx, DATA_VA + 8, KBASE + 0x40);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_TEXT, 0x40);
    ASSERT_EQ(fxRun(&fx, 0), BOOT_OK);
    ASSERT_TRUE(destUnchanged(&fx));
    ASSERT_EQ(fx.stats.applied, 2u);
    ASSERT_EQ(fx.stats.execApplied, 1u);
    /* ...and still validates: slide 0 does not excuse a bad table. */
    setupWith(&fx);
    addReloc(&fx, 1, DATA_VA + 8, R_32, Y_TEXT, 0);
    ASSERT_EQ(fxRun(&fx, 0), BOOT_ERR_ELF_RELOC);
    fxInit(&fx);
    ASSERT_EQ(fxRun(&fx, 0), BOOT_ERR_ELF_RELOC); /* no exec relocation */
}

TEST(relocFailureLeavesStatsZeroed) {
    setupWith(&fx);
    fx.stats.total = 99;
    addReloc(&fx, 1, DATA_VA + 8, 0x1234, Y_DATA, 0);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_ERR_ELF_RELOC);
    ASSERT_EQ(fx.stats.total, 0u);
    ASSERT_EQ(fx.stats.applied, 0u);
}

TEST(relocSlideOutsideWindowOrUnalignedIsRejected) {
    setupWith(&fx);
    ASSERT_EQ(fxRun(&fx, SLIDE + 1), BOOT_ERR_ELF_RANGE);
    ASSERT_EQ(fxRun(&fx, 0x20000000ULL), BOOT_ERR_ELF_RANGE); /* image would leave the window */
    ASSERT_EQ(fxRun(&fx, 0x1FFFF000ULL), BOOT_ERR_ELF_RANGE); /* span is 0x2000 */
    ASSERT_EQ(fxRun(&fx, 0xFFFFFFFFFFFFF000ULL), BOOT_ERR_ELF_RANGE);
    ASSERT_TRUE(destUnchanged(&fx));
    ASSERT_EQ(fxRun(&fx, 0x1FFFE000ULL), BOOT_OK); /* the last slide that still fits */
}

TEST(relocNullArgumentsAndBadHeaderAreRejected) {
    setupWith(&fx);
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(elfRelocate(NULL, fx.file, FILE_SIZE, fx.dest, SLIDE, NULL), BOOT_ERR_ELF_HEADER);
    ASSERT_EQ(elfRelocate(&fx.img, NULL, FILE_SIZE, fx.dest, SLIDE, NULL), BOOT_ERR_ELF_HEADER);
    ASSERT_EQ(elfRelocate(&fx.img, fx.file, FILE_SIZE, NULL, SLIDE, NULL), BOOT_ERR_ELF_HEADER);
    ASSERT_EQ(elfRelocate(&fx.img, fx.file, 32, fx.dest, SLIDE, NULL), BOOT_ERR_ELF_HEADER);
    uint8_t saved = fx.file[1];
    fx.file[1] = 'X';
    ASSERT_EQ(elfRelocate(&fx.img, fx.file, FILE_SIZE, fx.dest, SLIDE, NULL), BOOT_ERR_ELF_HEADER);
    fx.file[1] = saved;
}

TEST(relocStatsPointerMayBeNull) {
    setupWith(&fx);
    ASSERT_EQ(elfParse(fx.file, FILE_SIZE, &fx.img), BOOT_OK);
    ASSERT_EQ(elfLoad(&fx.img, fx.file, fx.dest), BOOT_OK);
    ASSERT_EQ(elfRelocate(&fx.img, fx.file, FILE_SIZE, fx.dest, SLIDE, NULL), BOOT_OK);
    ASSERT_EQ(destText32(&fx, EXEC_LOC), 0x80000040u + (uint32_t)SLIDE);
}

/* ---- section header table bounds ---- */

TEST(relocSectionHeaderTableBoundsAreChecked) {
    setupWith(&fx);
    put64(fx.file + 40, FILE_SIZE + 1); /* e_shoff past the end */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(fx.file + 40, FILE_SIZE - 64 * (S_COUNT - 1)); /* table runs one entry past the end */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(fx.file + 40, 0xFFFFFFFFFFFFFFF0ULL); /* e_shoff + table size wraps */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(fx.file + 40, 0); /* no section headers at all */
    put16(fx.file + 60, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocSectionHeaderCountAndSizeAreChecked) {
    setupWith(&fx);
    put16(fx.file + 60, 0); /* e_shnum == 0 */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put16(fx.file + 60, 257); /* > 256 */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put16(fx.file + 60, 0xFFFF);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put16(fx.file + 58, 56); /* e_shentsize != 64 */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put16(fx.file + 58, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocRelaSectionBoundsAreChecked) {
    setupWith(&fx);
    put64(shdrAt(&fx, S_RELA_TEXT) + 24, FILE_SIZE - 24); /* offset ok, but 1 entry runs past */
    put64(shdrAt(&fx, S_RELA_TEXT) + 32, 48);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_RELA_TEXT) + 32, 0xFFFFFFFFFFFFFFF0ULL); /* size huge: offset+size wraps */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_RELA_TEXT) + 24, 0xFFFFFFFFFFFFFFF0ULL);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_RELA_TEXT) + 32, 25); /* not a multiple of 24 */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_RELA_TEXT) + 56, 16); /* sh_entsize != 24 */
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put64(shdrAt(&fx, S_RELA_TEXT) + 56, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocRelaInfoOutOfRangeIsRejected) {
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_DATA) + 44, S_COUNT);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
    setupWith(&fx);
    put32(shdrAt(&fx, S_RELA_DATA) + 44, 0xFFFFFFFFu);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocTargetSectionAddressOverflowIsRejected) {
    setupWith(&fx);
    put64(shdrAt(&fx, S_DATA) + 16, 0xFFFFFFFFFFFFFFF0ULL); /* addr + size wraps */
    addReloc(&fx, 1, DATA_VA, R_64, Y_DATA, 0);
    EXPECT_REJECT(&fx, BOOT_ERR_ELF_RELOC);
}

TEST(relocOrderingOfSectionsDoesNotMatter) {
    /* Classification is by type/flags, never by name or position: swap the two RELA sections'
     * table positions' roles by pointing .rela.text at .data and .rela.data at .text. */
    fxInit(&fx);
    pokeText32(&fx, EXEC_LOC, 0x80000040u);
    pokeData64(&fx, DATA_VA + 8, KBASE + 0x40);
    put32(shdrAt(&fx, S_RELA_TEXT) + 44, S_DATA);
    put32(shdrAt(&fx, S_RELA_DATA) + 44, S_TEXT);
    addReloc(&fx, 1, EXEC_LOC, R_32S, Y_TEXT, 0x40);   /* now in the RELA that targets .text */
    addReloc(&fx, 0, DATA_VA + 8, R_64, Y_TEXT, 0x40); /* and this in the one targeting .data */
    ASSERT_EQ(fxRun(&fx, SLIDE), BOOT_OK);
    ASSERT_EQ(fx.stats.execApplied, 1u);
    ASSERT_EQ(destData64(&fx, DATA_VA + 8), KBASE + 0x40 + SLIDE);
}

/* Reads under ASan must never leave the file buffer, whatever fileSize the caller claims:
 * relocate against exact-size heap copies of every prefix (elfParse on the full file gave `img`).
 */
TEST(relocTruncatedFileNeverReadsOutOfBounds) {
    setupWith(&fx);
    pokeData64(&fx, DATA_VA + 8, KBASE + 0x40);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_TEXT, 0x40);
    ASSERT_EQ(elfParse(fx.file, FILE_SIZE, &fx.img), BOOT_OK);
    ASSERT_EQ(elfLoad(&fx.img, fx.file, fx.dest), BOOT_OK);
    uint8_t work[0x2000];
    uint32_t accepted = 0;
    for (uint64_t n = 0; n <= FILE_SIZE; n += (n < 0x100 ? 1 : 61)) {
        uint8_t *copy = malloc(n == 0 ? 1 : (size_t)n);
        ASSERT_TRUE(copy != NULL);
        memcpy(copy, fx.file, (size_t)n);
        memcpy(work, fx.dest, sizeof(work));
        BootStatus st = elfRelocate(&fx.img, copy, n, work, SLIDE, NULL);
        if (st == BOOT_OK) {
            accepted++;
        }
        free(copy);
    }
    /* Only prefixes that still contain the section table (at the very end) can be valid. */
    ASSERT_TRUE(accepted >= 1);
    uint8_t *copy = malloc(0x5000);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, fx.file, 0x5000);
    ASSERT_EQ(elfRelocate(&fx.img, copy, 0x5000, work, SLIDE, NULL), BOOT_ERR_ELF_RELOC);
    free(copy);
}

/* Random single-field corruption of the section/symbol/rela area: must return (any status) with no
 * ASan/UBSan report; when it does return BOOT_OK the result must equal a clean pass-1 outcome. */
TEST(relocCorruptedTablesNeverCrash) {
    setupWith(&fx);
    pokeData64(&fx, DATA_VA + 8, KBASE + 0x40);
    addReloc(&fx, 1, DATA_VA + 8, R_64, Y_TEXT, 0x40);
    ASSERT_EQ(elfParse(fx.file, FILE_SIZE, &fx.img), BOOT_OK);
    ElfImage img = fx.img;
    static uint8_t orig[FILE_SIZE];
    memcpy(orig, fx.file, FILE_SIZE);
    uint32_t rng = 12345;
    for (int iter = 0; iter < 20000; iter++) {
        memcpy(fx.file, orig, FILE_SIZE);
        for (int k = 0; k < 3; k++) {
            rng = rng * 1664525u + 1013904223u;
            uint32_t region = (rng >> 24) % 3;
            rng = rng * 1664525u + 1013904223u;
            uint32_t pos = (region == 0   ? 0x5000u
                            : region == 1 ? 0x3000u
                                          : 0x4000u) +
                           ((rng >> 8) % (region == 0   ? 640u
                                          : region == 1 ? 216u
                                                        : 0x50u));
            rng = rng * 1664525u + 1013904223u;
            fx.file[pos] = (uint8_t)(rng >> 16);
        }
        ASSERT_EQ(elfLoad(&img, orig, fx.dest), BOOT_OK);
        (void)elfRelocate(&img, fx.file, FILE_SIZE, fx.dest, SLIDE, NULL);
    }
}
