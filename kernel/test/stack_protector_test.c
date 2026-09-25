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

/* Deliberately overflows `buf` into the adjacent canary -- 24 bytes clears the 8-byte canary with
 * margin while staying inside this function's own frame (buf, the canary, and this function's own
 * saved RBP; its own return address is left alone). Originally chosen because it also had to
 * avoid reaching archTrapCatchCall's frame above this one: an earlier version of this test used 64
 * bytes and clobbered the stack slot `archTrapCatchResume`'s `ret` depended on, turning the catch
 * into a real #GP instead (see docs/logs/M2.1.md). archTrapCatchCall/archTrapCatchResume
 * (trap-entry.asm) no longer depend on that slot at all -- the resume target is saved in
 * `TrapCatchCtx` itself now -- but 24 bytes is still plenty to prove the point without smashing
 * any more of the stack than necessary. */
static void stackSmashTrigger(void *arg) {
    (void)arg;
    char buf[8];
    memset(buf, 0x41, 24);
}

/* Checks that __stack_chk_fail() offers the trip to archTrapCatch (rather than only ever
 * panicking) and that the captured RIP -- __stack_chk_fail's own return address -- symbolizes
 * back to stackSmashTrigger's epilogue, confirming TrapCatchInfo carries real data through the
 * software-trip path (archTrapCatchSoftware), not just the hardware-fault one the other trap
 * tests exercise. Symbolizes `info.rip - 1`, not `info.rip` directly: it's a return address, and
 * __stack_chk_fail's call to it is stackSmashTrigger's own last statement before its (never
 * reached) normal epilogue, so the raw return address can land on whatever function the linker
 * placed right after stackSmashTrigger instead -- the same convention kernel/core/backtrace.c's
 * printFrame() already uses for every other return-address backtrace frame (arch/trap.h). */
KTEST(stack_protector_detects_smash) {
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_STACK_SMASH, stackSmashTrigger, NULL, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_STACK_SMASH);

    char sym[80];
    ksymSymbolize(info.rip - 1, sym, sizeof(sym));
    KTEST_ASSERT(cmdlineGlobMatch("stackSmashTrigger+*", sym));
}
