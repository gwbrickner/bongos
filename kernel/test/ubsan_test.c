/* ktest for the UBSan runtime actually catching a trip (ARCHITECTURE §3, D-076/D-078). The
 * handlers themselves (kernel/core/ubsan.c) were verified in the previous step by confirming
 * nothing in the existing kernel/ktest code spuriously trips one; this is the first test that
 * deliberately triggers one and proves the report/catch path works. Debug-only (KERNEL_UBSAN,
 * mk/kernel.mk) -- there is nothing to catch in a RELEASE build, where UBSan instrumentation
 * isn't compiled in at all. */
#include "cmdline.h"
#include "ksym.h"
#include "ktest.h"

#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

#if KERNEL_UBSAN

/* `volatile` on both locals: without it, a constant-folding compiler could evaluate `INT32_MAX +
 * 1` at compile time (still UB, but never reaching the instrumented runtime add at all) instead
 * of emitting the runtime check this test means to exercise. */
static void ubsanOverflowTrigger(void *arg) {
    (void)arg;
    volatile int32_t a = INT32_MAX;
    volatile int32_t b = 1;
    volatile int32_t r = a + b;
    (void)r;
}

/* Checks that report() offers the trip to archTrapCatch (rather than only ever panicking) and
 * that the captured RIP is a real, symbolizable kernel address -- proving TrapCatchInfo carries
 * real data through the UBSan software-trip path, the same property stack_protector_detects_smash
 * checks for __stack_chk_fail. Doesn't assert *which* symbol it resolves to (unlike that test and
 * trap_ud_caught, which pin down the exact function): report()'s __builtin_return_address(0) is
 * its *caller's* return address, i.e. wherever __ubsan_handle_add_overflow's own call to report()
 * returns to -- and since that call is __ubsan_handle_add_overflow's last statement and report()
 * never returns, there's no epilogue after it, so that address numerically coincides with
 * whatever function the linker happened to place right after __ubsan_handle_add_overflow in
 * .text (observed: report() itself, i.e. "report+0x0") rather than anywhere inside
 * ubsanOverflowTrigger. That placement is a compiler/linker implementation detail this test
 * shouldn't pin down; a resolvable symbol (not ksymSymbolize's "?" not-found fallback) is what
 * actually proves the RIP is real. */
KTEST(ubsan_catches_signed_overflow) {
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_UBSAN, ubsanOverflowTrigger, NULL, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_UBSAN);

    char sym[80];
    ksymSymbolize(info.rip, sym, sizeof(sym));
    KTEST_ASSERT(!cmdlineStrEq(sym, "?"));
}

#endif /* KERNEL_UBSAN */
