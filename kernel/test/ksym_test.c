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

static uint64_t capturedFp;

static void __attribute__((noinline)) chainC(void) {
    capturedFp = archFramePointer();
}
static void __attribute__((noinline)) chainB(void) {
    chainC();
}
static void __attribute__((noinline)) chainA(void) {
    chainB();
}

/* A→B→C call chain: backtraceCapture() from inside C should walk back through B's and A's return
 * addresses, then this test function's own -- proving the walk (and, via the glob checks, the
 * addr-1 symbolization convention backtracePrint() uses for return addresses) actually chains
 * through real call frames, not just that it doesn't crash. */
KTEST(backtrace_symbolized) {
    chainA();

    uint64_t addrs[8];
    size_t n = backtraceCapture(capturedFp, addrs, 8);
    KTEST_ASSERT(n >= 3);

    char buf[80];
    /* Each entry is a return address (points *after* the call instruction); backtracePrint()'s
     * own convention is to symbolize at addr-1 so a call to the very last instruction of the
     * calling function still attributes to that function, not whatever follows it -- matched
     * here so the test proves the same convention that ships in the real panic/trap report. */
    ksymSymbolize(addrs[0] - 1, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("chainB+*", buf));

    ksymSymbolize(addrs[1] - 1, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("chainA+*", buf));

    ksymSymbolize(addrs[2] - 1, buf, sizeof(buf));
    KTEST_ASSERT(cmdlineGlobMatch("ktestFn_backtrace_symbolized+*", buf));
}
