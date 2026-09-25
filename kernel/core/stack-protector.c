/* Stack-protector runtime for -fstack-protector-strong (ARCHITECTURE §21). Two symbols the
 * compiler references by these exact, non-prefixed names (same exception as string.c's
 * memcpy/memset/memmove/memcmp): `__stack_chk_guard`, read by every protected function's
 * prologue/epilogue, and `__stack_chk_fail`, called when the canary doesn't match.
 *
 * M1.3: the guard is a fixed nonzero compile-time constant -- there is no RNG yet. M2.1 reseeds it
 * from BootInfo.randomSeed in a function marked `__attribute__((no_stack_protector))` (so seeding
 * it isn't itself a protected frame), done before any protected frame that outlives the reseed. */
#include "panic.h"

#include <stdint.h>

uintptr_t __stack_chk_guard = 0xBADC0FFEE0DDF00DULL;

/* No locks; never returns (panics). May be called from any protected function's epilogue. */
__attribute__((noreturn)) void __stack_chk_fail(void) {
    panic("stack protector: smashed");
}
