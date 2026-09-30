/* kaslrcheck: build-time proof that the loader's relocator (boot/common/elf-reloc.c) is exactly
 * equivalent to linking the kernel at a different base (M2.6, D-120).
 *
 *   kaslrcheck <kernel.elf> <kernel.alt.elf> <slide>
 *
 * kernel.alt.elf is the same objects relinked with KERNEL_LINK_BASE = link base + slide (mk/
 * kernel.mk). The tool runs the loader's own elfParse/elfLoad/elfRelocate on kernel.elf with
 * `slide` and requires the resulting image to be byte-identical to elfLoad of the alt link (and the
 * entry point, segment layout and relocation counts to agree). Any relocation type the relocator
 * gets wrong, skips, or applies twice shows up as a differing byte. Also checks that slide 0 leaves
 * the loaded image untouched. Host tool (ARCHITECTURE §0), built from boot/common's own sources. */
#include "elf64.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Noreturn void die(const char *what, const char *arg) {
    fprintf(stderr, "kaslrcheck: FAIL: %s%s%s\n", what, arg != NULL ? ": " : "",
            arg != NULL ? arg : "");
    exit(1);
}

static uint8_t *readFile(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        die("cannot open", path);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        die("seek failed", path);
    }
    long n = ftell(f);
    if (n < 64) {
        die("too small to be an ELF file", path);
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        die("read failed", path);
    }
    fclose(f);
    *size = (uint64_t)n;
    return buf;
}

typedef struct {
    uint8_t *file;
    uint64_t fileSize;
    ElfImage img;
    uint8_t *dest;
} Kernel;

static void loadKernel(Kernel *k, const char *path) {
    k->file = readFile(path, &k->fileSize);
    BootStatus st = elfParse(k->file, k->fileSize, &k->img);
    if (st != BOOT_OK) {
        die("elfParse rejected", path);
    }
    k->dest = malloc((size_t)k->img.span);
    if (k->dest == NULL) {
        die("out of memory", NULL);
    }
    st = elfLoad(&k->img, k->file, k->dest);
    if (st != BOOT_OK) {
        die("elfLoad failed", path);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: kaslrcheck <kernel.elf> <kernel.alt.elf> <slide>\n");
        return 2;
    }
    char *end = NULL;
    uint64_t slide = strtoull(argv[3], &end, 0);
    if (end == argv[3] || *end != '\0' || slide == 0) {
        die("slide must be a non-zero number", argv[3]);
    }

    Kernel base, alt;
    loadKernel(&base, argv[1]);
    loadKernel(&alt, argv[2]);

    if (alt.img.linkBase != base.img.linkBase + slide) {
        die("alt link base is not base + slide", NULL);
    }
    if (alt.img.entry != base.img.entry + slide) {
        die("alt entry != entry + slide", NULL);
    }
    if (alt.img.span != base.img.span || alt.img.segCount != base.img.segCount) {
        die("alt link has a different span or segment count", NULL);
    }
    for (uint32_t i = 0; i < base.img.segCount; i++) {
        if (alt.img.segs[i].vaddr != base.img.segs[i].vaddr + slide ||
            alt.img.segs[i].memsz != base.img.segs[i].memsz ||
            alt.img.segs[i].filesz != base.img.segs[i].filesz ||
            alt.img.segs[i].flags != base.img.segs[i].flags) {
            die("alt link has a different segment layout", NULL);
        }
    }

    /* slide 0: validates the table, must not change a byte. */
    ElfRelocStats zeroStats;
    uint8_t *copy = malloc((size_t)base.img.span);
    if (copy == NULL) {
        die("out of memory", NULL);
    }
    memcpy(copy, base.dest, (size_t)base.img.span);
    if (elfRelocate(&base.img, base.file, base.fileSize, base.dest, 0, &zeroStats) != BOOT_OK) {
        die("elfRelocate(slide 0) rejected kernel.elf", NULL);
    }
    if (memcmp(copy, base.dest, (size_t)base.img.span) != 0) {
        die("elfRelocate(slide 0) modified the image", NULL);
    }

    ElfRelocStats stats;
    BootStatus st = elfRelocate(&base.img, base.file, base.fileSize, base.dest, slide, &stats);
    if (st != BOOT_OK) {
        fprintf(stderr, "kaslrcheck: elfRelocate status %d (%s)\n", (int)st, bootStatusString(st));
        die("elfRelocate rejected kernel.elf", NULL);
    }
    if (stats.applied == 0 || stats.execApplied == 0 || stats.applied != zeroStats.applied) {
        die("implausible relocation counts", NULL);
    }

    /* Report every differing byte range (first 8) so a wrong relocation type is easy to find. */
    uint64_t diffs = 0;
    for (uint64_t i = 0; i < base.img.span; i++) {
        if (base.dest[i] != alt.dest[i]) {
            if (diffs < 8) {
                fprintf(stderr,
                        "kaslrcheck: image offset 0x%" PRIx64 " (va 0x%" PRIx64
                        "): relocated 0x%02x, relinked 0x%02x\n",
                        i, base.img.linkBase + i, base.dest[i], alt.dest[i]);
            }
            diffs++;
        }
    }
    if (diffs != 0) {
        fprintf(stderr, "kaslrcheck: %" PRIu64 " byte(s) differ\n", diffs);
        die("relocated kernel.elf differs from the relinked kernel.alt.elf", NULL);
    }

    printf("kaslrcheck: OK: slide 0x%" PRIx64 ", %" PRIu32 " relocs (%" PRIu32 " slid, %" PRIu32
           " in code, %" PRIu32 " skipped) in %" PRIu32 " sections; %" PRIu64
           " bytes identical to the relinked kernel\n",
           slide, stats.total, stats.applied, stats.execApplied, stats.skipped, stats.relaSections,
           base.img.span);
    return 0;
}
