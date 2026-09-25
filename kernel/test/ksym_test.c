/* ktests for KSYM v1 symbolization (ARCHITECTURE §24, D-075): kernel/core/ksym.c's
 * ksymDecodeLookup()/ksymSymbolize() and kernel/core/backtrace.c's backtraceCapture(), exercised
 * directly (no fault needed -- these don't depend on archTrapCatch, a later M2.1 step). */
#include "backtrace.h"
#include "cmdline.h"
#include "ksym.h"
#include "ktest.h"
#include "panic.h"

#include <arch/cpu.h>
#include <stdint.h>

/* Any real, address-taken kernel function works: panic() is declared everywhere and its address
 * is never inlined away (address-of forces a real, out-of-line symbol). */
KTEST(ksym_lookup) {
    uint64_t addr = (uint64_t)(uintptr_t)&panic;
    char buf[80];

    ksymSymbolize(addr, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("panic+0x0", buf));

    ksymSymbolize(addr + 5, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("panic+0x5", buf));

    /* Address 0 is far outside [kernelTextStart, kernelTextEnd) -- must report "not found", never
     * fault or return a bogus name. */
    ksymSymbolize(0, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineStrEq(buf, "?"));
}

static uint64_t capturedAddrs[8];
static size_t capturedCount;

/* Deliberately captures the walk *from inside* chainC, before it (or chainB/chainA) returns: once
 * a function returns, the stack memory its frame occupied is free for reuse, and the very next
 * call in the caller (backtraceCapture() itself, if called there instead) would overwrite exactly
 * the frame data being walked before it could ever be read -- confirmed by hand with a debug dump
 * showing a captured frame pointer's saved-rbp/return-address slot already clobbered by the
 * caller's own next call. Capturing while the chain is still live sidesteps that: only the plain
 * uint64_t addresses need to survive past the return, not the stack memory itself. */
static void __attribute__((noinline)) chainC(void) {
    capturedCount = backtraceCapture(archFramePointer(), capturedAddrs, 8);
}
/* `noinline` alone stops these calls from being inlined, but not from being *tail-called*: at
 * -O1, a function whose last (and only) action is calling another with a compatible signature is
 * a sibling-call candidate, which clang turns into a `jmp` reusing the caller's own stack frame
 * instead of a `call` that pushes a new return address -- confirmed by hand via objdump (chainA/
 * chainB compiled to `push rbp; mov rsp,rbp; pop rbp; jmp chainB/chainC`, collapsing the intended
 * 3-deep call chain into a single frame). The empty `asm volatile("")` after each call gives the
 * compiler something (nothing, but *something*) it must still do afterward, so the call can no
 * longer be in tail position. */
static void __attribute__((noinline)) chainB(void) {
    chainC();
    __asm__ volatile("");
}
static void __attribute__((noinline)) chainA(void) {
    chainB();
    __asm__ volatile("");
}

/* A→B→C call chain: the walk captured from inside C should contain C's own return address (into
 * B), B's (into A), and A's (into this test function), proving the walk chains through real call
 * frames and that symbolization correctly attributes each one, not just "does it not crash". */
KTEST(backtrace_symbolized) {
    chainA();
    KTEST_ASSERT(capturedCount >= 3);

    char buf[80];
    /* Each entry is a return address (points *after* the call instruction); backtracePrint()'s
     * own convention is to symbolize at addr-1 so a call to the very last instruction of the
     * calling function still attributes to that function, not whatever follows it -- matched
     * here so the test proves the same convention that ships in the real panic/trap report. */
    ksymSymbolize(capturedAddrs[0] - 1, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("chainB+*", buf));

    ksymSymbolize(capturedAddrs[1] - 1, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("chainA+*", buf));

    ksymSymbolize(capturedAddrs[2] - 1, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("ktestFn_backtrace_symbolized+*", buf));
}
