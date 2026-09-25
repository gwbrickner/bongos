/* ktests for the IDT/trap dispatch path (ARCHITECTURE §7.2/§23, D-074) and archTrapCatch
 * (ARCHITECTURE §23, D-078). */
#include "cmdline.h"
#include "ksym.h"
#include "ktest.h"
#include "sections.h"

#include <arch/trap.h>
#include <stdint.h>

/* ROADMAP M2.1's "int3 resumes": #BP is a trap, not a fault, so trapCommon's iretq must land
 * right after the `int3` byte with every general-purpose register exactly as it was -- checked
 * here directly (not just "did execution continue") since trapCommon's push/pop symmetry across
 * the C call into trapDispatch() is exactly the kind of thing that silently breaks under a
 * one-register reordering mistake. */
KTEST(trap_int3_resumes) {
    uint64_t before = archBreakpointHits();

    uint64_t rax, rbx, rcx, r15;
    __asm__ volatile("mov $0x1111111111111111, %%rax\n\t"
                     "mov $0x2222222222222222, %%rbx\n\t"
                     "mov $0x3333333333333333, %%rcx\n\t"
                     "mov $0x4444444444444444, %%r15\n\t"
                     "int3\n\t"
                     "mov %%rax, %0\n\t"
                     "mov %%rbx, %1\n\t"
                     "mov %%rcx, %2\n\t"
                     "mov %%r15, %3\n\t"
                     : "=r"(rax), "=r"(rbx), "=r"(rcx), "=r"(r15)
                     :
                     : "rax", "rbx", "rcx", "r15");

    KTEST_ASSERT_EQ(rax, 0x1111111111111111ULL);
    KTEST_ASSERT_EQ(rbx, 0x2222222222222222ULL);
    KTEST_ASSERT_EQ(rcx, 0x3333333333333333ULL);
    KTEST_ASSERT_EQ(r15, 0x4444444444444444ULL);
    KTEST_ASSERT_EQ(archBreakpointHits(), before + 1);
}

/* `volatile` on both the pointer and the access: without it, a smart-enough compiler could prove
 * the read/write is dead (nothing observes the result) and drop it entirely, which would silently
 * turn these tests into no-ops instead of the deliberate #PF they're meant to trigger. */
static void trapPfReadTrigger(void *arg) {
    volatile const uint8_t *p = (volatile const uint8_t *)arg;
    volatile uint8_t v = *p;
    (void)v;
}

static void trapPfWriteTrigger(void *arg) {
    volatile uint8_t *p = (volatile uint8_t *)arg;
    *p = 0x42;
}

/* ROADMAP M2.1's "page fault reports CR2 correctly": a read 8 bytes below the boot stack's bottom
 * lands squarely in kernel.ld's unmapped guard page (a full page, D-073, so any offset short of a
 * whole page below the bottom works equally well), so this is a real #PF, not a simulated one.
 * Checks CR2 (SDM Vol 3A §4.7: holds the exact faulting linear address) and the error code's P bit
 * (0: the guard page has no mapping at all, so the fault is "not present", not a permission
 * violation) and W bit (0: this is a read). */
KTEST(trap_pf_read_cr2) {
    const uint8_t *guardAddr = kernelBootStackBottom - 8;
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_VEC(14), trapPfReadTrigger, (void *)guardAddr, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_VEC(14));
    KTEST_ASSERT_EQ(info.vector, 14);
    KTEST_ASSERT_EQ(info.cr2, (uint64_t)(uintptr_t)guardAddr);
    KTEST_ASSERT_EQ(info.errorCode & 0x3, 0); /* P=0 (not present), W=0 (read) */
}

/* Same guard page, but a write -- SDM Vol 3A §4.7's W bit (bit 1) reflects the *access type* that
 * caused the fault independently of the P bit, so a write to an unmapped page still reports W=1
 * even though P=0 (there's no page there to have been read-only). */
KTEST(trap_pf_write_error_code) {
    uint8_t *guardAddr = (uint8_t *)(kernelBootStackBottom - 16);
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_VEC(14), trapPfWriteTrigger, guardAddr, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.vector, 14);
    KTEST_ASSERT_EQ(info.cr2, (uint64_t)(uintptr_t)guardAddr);
    KTEST_ASSERT_EQ(info.errorCode & 0x3, 0x2); /* P=0 (not present), W=1 (write) */
}

/* D-073: IST1/2/3 (#DF/NMI/#MC) each get their own guard page below them too, for exactly the
 * same reason the boot stack does -- a stack overflow into an unmapped page is a loud #PF, not
 * silent corruption of whatever memory happens to sit below. Checked directly rather than taken
 * on faith from the linker script. */
KTEST(stack_guards_unmapped) {
    const uint8_t *const bottoms[3] = {kernelIst1Bottom, kernelIst2Bottom, kernelIst3Bottom};
    for (int i = 0; i < 3; i++) {
        const uint8_t *guardAddr = bottoms[i] - 8;
        TrapCatchInfo info;
        bool caught =
            archTrapCatch(TRAP_CATCH_VEC(14), trapPfReadTrigger, (void *)guardAddr, &info);
        KTEST_ASSERT(caught);
        KTEST_ASSERT_EQ(info.vector, 14);
        KTEST_ASSERT_EQ(info.cr2, (uint64_t)(uintptr_t)guardAddr);
    }
}

/* `ud2` is the architectural "always #UD" instruction (SDM Vol 2A), so this needs no contrived
 * setup -- unlike the guard-page tests, the *instruction* itself is the fault, not an operand. */
static void trapUdTrigger(void *arg) {
    (void)arg;
    __asm__ volatile("ud2");
}

/* Checks that the caught RIP is exactly the faulting instruction (SDM Vol 3A §6.15: #UD doesn't
 * advance RIP, unlike a trap) and that it symbolizes back to this very function -- proving
 * archTrapCatchTryResume() hands trapDispatch()'s *original* frame data through TrapCatchInfo
 * rather than something already clobbered by the resume. */
KTEST(trap_ud_caught) {
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_VEC(6), trapUdTrigger, NULL, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_VEC(6));
    KTEST_ASSERT_EQ(info.vector, 6);

    char sym[80];
    ksymSymbolize(info.rip, sym, sizeof(sym));
    KTEST_ASSERT(cmdlineGlobMatch("trapUdTrigger+*", sym));
}
