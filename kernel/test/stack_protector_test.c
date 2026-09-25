/* ktests for the stack-protector reseed (ARCHITECTURE §6.6, D-077) and, via archTrapCatch
 * (D-078), the canary check itself actually catching a smash. */
#include "cmdline.h"
#include "ksym.h"
#include "ktest.h"
#include "stack-protector.h"

#include <arch/trap.h>
#include <stdbool.h>
#include <stddef.h>

KTEST(stack_guard_seeded) {
    /* The M1.3-era fixed constant, still __stack_chk_guard's initial value until stackGuardInit()
     * runs -- by the time any ktest executes, kernelMain has long since called it. */
    KTEST_ASSERT(__stack_chk_guard != 0xBADC0FFEE0DDF00DULL);

    /* D-077: the low byte is always forced to 0 (the canary doubles as a string terminator
     * against a buffer over-read), and the guard is never left at exactly 0. */
    KTEST_ASSERT((__stack_chk_guard & 0xFF) == 0);
    KTEST_ASSERT(__stack_chk_guard != 0);
}

/* Freestanding: no <string.h>; kernel/core/string.c is the one place that keeps memset's plain
 * libc name (ARCHITECTURE §4), so calling it here just needs a local declaration like any other C
 * function. memset's own body writes through an opaque `uint8_t*` parameter with no compile-time-
 * known array size, so -fsanitize=bounds can't catch this overflow before it ever reaches the
 * canary -- unlike a direct `buf[i] = ...` on a fixed-size array, which the bounds sanitizer
 * would trip on first and defeat the point of this test. */
extern void *memset(void *dst, int value, size_t n);

/* Deliberately overflows `buf` into the adjacent canary -- but by a *bounded* amount, not an
 * arbitrary one: `archTrapCatch`'s catch path resumes via a register snapshot taken in global
 * memory (trapCatch.ctx, trap.c), safe from any stack corruption, but that snapshot's saved RSP
 * still points at a real stack slot (archTrapCatchCall's own return address, trap-entry.asm) that
 * its final `ret` reads *from the stack* at resume time. A wild overflow big enough to reach past
 * this function's own frame into archTrapCatchCall's would clobber that exact slot with 0x41
 * bytes -- a non-canonical address -- turning the resume's `ret` into a #GP instead of a clean
 * catch. 24 bytes clears the 8-byte canary with margin while staying entirely inside this
 * function's own frame (buf, the canary, and this function's own saved RBP -- never read again
 * once the canary mismatch is detected, since that path calls __stack_chk_fail directly rather
 * than falling through to this function's own `leave`/`ret`). */
static void stackSmashTrigger(void *arg) {
    (void)arg;
    char buf[8];
    memset(buf, 0x41, 24);
}

/* Checks that __stack_chk_fail() offers the trip to archTrapCatch (rather than only ever
 * panicking) and that the captured RIP -- __stack_chk_fail's own return address -- symbolizes
 * back to stackSmashTrigger's epilogue, confirming TrapCatchInfo carries real data through the
 * software-trip path (archTrapCatchSoftware), not just the hardware-fault one the other trap
 * tests exercise. */
KTEST(stack_protector_detects_smash) {
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_STACK_SMASH, stackSmashTrigger, NULL, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_STACK_SMASH);

    char sym[80];
    ksymSymbolize(info.rip, sym, sizeof(sym));
    KTEST_ASSERT(cmdlineGlobMatch("stackSmashTrigger+*", sym));
}
