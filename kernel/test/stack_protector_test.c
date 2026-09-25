/* ktests for the stack-protector reseed (ARCHITECTURE §6.6, D-077). Actually smashing a stack and
 * catching the resulting panic needs `archTrapCatch` (D-078, not built yet) -- that ktest,
 * `stack_protector_detects_smash`, lands in the step that adds it. */
#include "ktest.h"
#include "stack-protector.h"

KTEST(stack_guard_seeded) {
    /* The M1.3-era fixed constant, still __stack_chk_guard's initial value until stackGuardInit()
     * runs -- by the time any ktest executes, kernelMain has long since called it. */
    KTEST_ASSERT(__stack_chk_guard != 0xBADC0FFEE0DDF00DULL);

    /* D-077: the low byte is always forced to 0 (the canary doubles as a string terminator
     * against a buffer over-read), and the guard is never left at exactly 0. */
    KTEST_ASSERT((__stack_chk_guard & 0xFF) == 0);
    KTEST_ASSERT(__stack_chk_guard != 0);
}
