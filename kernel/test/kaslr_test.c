/* Portable KASLR ktest (M2.6, ARCHITECTURE §5.5/§6.6): the KSYM blob accounts for the slide. The
 * x86-specific proofs (slide agrees with where the image runs, relocations applied) live in
 * kernel/arch/x86_64/test/kaslr_arch_test.c because they need inline assembly. */
#include "cmdline.h"
#include "ksym.h"
#include "ktest.h"
#include "panic.h"
#include "sections.h"

#include <stdint.h>

#define KASLR_TEST_EXTRA_SLIDE 0x200000ULL /* one 2 MiB slot beyond whatever the real slide is */

/* The KSYM blob keeps link-time addresses; runtime addresses must be mapped back by exactly
 * kernelSlide() (ksymDecodeLookupSlid). A lookup that ignored the slide would find nothing (or the
 * wrong symbol) as soon as the image moves, so this also pins the "add the slide back to symAddr"
 * half: the offset printed in a backtrace is `addr - symAddr`. The synthetic extra slide exercises
 * a nonzero slide even on a boot where the loader chose 0 (or kaslr = off). */
KTEST(ksym_slide_accounted) {
    size_t blobSize = (size_t)(ksymsEnd - ksymsStart);
    uint64_t panicAddr = (uint64_t)(uintptr_t)&panic;
    char name[64];
    uint64_t symAddr = 0;

    /* The real slide of this boot. */
    KTEST_ASSERT(ksymDecodeLookupSlid(ksymsStart, blobSize, panicAddr + 5, kernelSlide(), name,
                                      sizeof(name), &symAddr) == STATUS_OK);
    KTEST_ASSERT(cmdlineStrEq(name, "panic"));
    KTEST_ASSERT_EQ(symAddr, panicAddr);

    /* The same symbol pretending the image had been slid EXTRA further: runtime addresses and
     * symbol address both move by the extra slide, the name and the offset do not. */
    uint64_t slide2 = kernelSlide() + KASLR_TEST_EXTRA_SLIDE;
    symAddr = 0;
    KTEST_ASSERT(ksymDecodeLookupSlid(ksymsStart, blobSize, panicAddr + KASLR_TEST_EXTRA_SLIDE + 5,
                                      slide2, name, sizeof(name), &symAddr) == STATUS_OK);
    KTEST_ASSERT(cmdlineStrEq(name, "panic"));
    KTEST_ASSERT_EQ(symAddr, panicAddr + KASLR_TEST_EXTRA_SLIDE);

    /* An address below the slide can never be a runtime address of the image: NOT_FOUND, and
     * `symAddr` is left untouched. */
    symAddr = 0x1234;
    KTEST_ASSERT(ksymDecodeLookupSlid(ksymsStart, blobSize, 0, slide2, name, sizeof(name),
                                      &symAddr) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT_EQ(symAddr, 0x1234);
}
