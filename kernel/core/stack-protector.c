/* Stack-protector runtime for -fstack-protector-strong (ARCHITECTURE §21/§6.6, D-077). Two
 * symbols the compiler references by these exact, non-prefixed names (same exception as
 * string.c's memcpy/memset/memmove/memcmp): `__stack_chk_guard`, read by every protected
 * function's prologue/epilogue, and `__stack_chk_fail`, called when the canary doesn't match. */
#include "bootinfo.h"
#include "panic.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stdint.h>

/* M1.3-era fixed constant until stackGuardInit() runs; kept as the initial value so any protected
 * frame that somehow ran before kernelMain calls stackGuardInit() (there shouldn't be one) still
 * fails loudly on a real mismatch instead of silently matching an all-zero guard. */
uintptr_t __stack_chk_guard = 0xBADC0FFEE0DDF00DULL;

static bool guardSeeded = false;

/* SplitMix64's finalizer (a public-domain, well-known integer hash -- not a cryptographic PRNG,
 * just decent bit mixing for folding several input words into one canary value before the real
 * CSPRNG exists, M2.6). */
static uint64_t mix64(uint64_t z) {
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ULL;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return z;
}

static uint64_t readLe64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* Sets __stack_chk_guard exactly once, from a fold of every qword of BootInfo.randomSeed (64
 * bytes) mixed with one `rdtsc` reading -- there is no real CSPRNG yet (M2.6). Both `noinline`
 * and `no_stack_protector`: a compiler probe confirmed `no_stack_protector` alone doesn't stop
 * clang from inlining this into a protected caller, which would then fail its own canary check on
 * return since the reseed happens partway through that caller's (now-checked) frame. Must be
 * called from kernelMain (itself `no_stack_protector`) before any other protected frame that
 * outlives it -- every protected function that ran earlier already returned and checked the old
 * constant guard. Never reseeded again afterward: M2.6's CSPRNG consumes and wipes the live seed
 * instead. Panics if called twice. No locks, boot-time only. */
__attribute__((noinline, no_stack_protector)) void stackGuardInit(const BootInfo *bi) {
    if (guardSeeded) {
        panic("stack-protector: stackGuardInit() called twice");
    }
    guardSeeded = true;

    uint64_t h = 0x6A09E667F3BCC908ULL ^ archReadTsc();
    for (int i = 0; i < 8; i++) {
        h = mix64(h ^ readLe64(bi->randomSeed + 8 * i));
    }

    /* Low byte forced to 0: the canary also acts as a string terminator against a buffer
     * over-read (the classic reason real stack-protector guards always have a NUL byte), and
     * "never exactly 0" guards against the all-zero degenerate case that would make the canary
     * check trivially always pass. */
    uint64_t guard = h & ~0xFFULL;
    if ((guard >> 8) == 0) {
        guard = 0x5A5A5A5A5A5A5A00ULL;
    }
    __stack_chk_guard = guard;
}

/* No locks; never returns (panics). May be called from any protected function's epilogue. */
__attribute__((noreturn)) void __stack_chk_fail(void) {
    panic("stack protector: smashed");
}
