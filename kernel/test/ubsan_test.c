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
 * that the captured RIP symbolizes back to ubsanOverflowTrigger, the same property
 * stack_protector_detects_smash checks for __stack_chk_fail. This only pins down the exact
 * function because kernel/core/ubsan.c's report() now takes `pc` as an explicit parameter,
 * measured at each `noinline` __ubsan_handle_*'s own call site (its own
 * __builtin_return_address(0)) rather than inside the shared report() helper itself -- report()
 * calling __builtin_return_address(0) directly would only ever see *its own* caller (the
 * `__ubsan_handle_*` trampoline), never the original instrumented site. Symbolizes `info.rip - 1`,
 * not `info.rip` directly: it's a return address, and the compiler's instrumentation call is
 * itself the last thing __ubsan_handle_add_overflow does before its own epilogue, so the raw
 * return address can in principle still land on whatever the linker placed right after it -- the
 * same convention kernel/core/backtrace.c's printFrame() already uses for every other
 * return-address backtrace frame (arch/trap.h). */
KTEST(ubsan_catches_signed_overflow) {
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_UBSAN, ubsanOverflowTrigger, NULL, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_UBSAN);

    char sym[80];
    ksymSymbolize(info.rip - 1, sym, sizeof(sym));
    KTEST_ASSERT(cmdlineGlobMatch("ubsanOverflowTrigger+*", sym));
}

#endif /* KERNEL_UBSAN */
