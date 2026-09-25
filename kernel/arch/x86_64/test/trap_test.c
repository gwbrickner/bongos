/* ktests for the IDT/trap dispatch path (ARCHITECTURE §7.2/§23, D-074). */
#include "ktest.h"

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
