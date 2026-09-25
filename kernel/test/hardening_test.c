/* Portable hardening ktests: the stack protector and (when built in) UBSan (ARCHITECTURE
 * §6.6/§21, D-074/D-075/D-076). */
#include "ktest.h"

#include <arch/jmp.h>
#include <stdint.h>

/* Generous on purpose: guaranteed to reach the canary regardless of exactly how clang pads
 * around a 16-byte local array. */
static volatile uint32_t smashOverflowBytes = 64;

__attribute__((noinline)) static void ktestSmashVictim(void) {
    char buf[16];
    char *p = buf;
    /* Launder the pointer so UBSan's own object-size/bounds checks (KERNEL_UBSAN debug builds)
     * can't see through it and report first -- this test is specifically about the stack
     * protector's own detection, not UBSan's. */
    __asm__ volatile("" : "+r"(p));
    for (uint32_t i = 0; i < smashOverflowBytes; i++) {
        p[i] = 0x41;
    }
}

__attribute__((noinline)) static void ktestSmashOuter(void) {
    volatile char pad[256]; /* keeps the smash from also reaching this frame's own data/canary */
    pad[0] = 0;
    ktestSmashVictim();
}

KTEST(stack_smash_detected) {
    ktestArmExpectedPanic(ktestCtx, "stack protector");
    if (archJmpSave(ktestPanicJmpBuf()) == 0) {
        ktestSmashOuter();
        KTEST_ASSERT(false); /* unreachable: the smash panics before ktestSmashOuter returns */
    }
}

#if KERNEL_UBSAN
KTEST(ubsan_overflow_detected) {
    ktestArmExpectedPanic(ktestCtx, "ubsan: signed integer overflow");
    if (archJmpSave(ktestPanicJmpBuf()) == 0) {
        volatile int v = 0x7FFFFFFF; /* INT_MAX, without <limits.h> (not a freestanding header) */
        v = v + 1;
        (void)v;
        KTEST_ASSERT(false); /* unreachable */
    }
}
#endif
