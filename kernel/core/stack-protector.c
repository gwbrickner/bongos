/* Stack-protector runtime for -fstack-protector-strong (ARCHITECTURE §6.6/§21, D-074). Two
 * symbols the compiler references by these exact, non-prefixed names (same exception as
 * string.c's memcpy/memset/memmove/memcmp): `__stack_chk_guard`, read by every protected
 * function's prologue/epilogue, and `__stack_chk_fail`, called when the canary doesn't match.
 * Plus stackGuardInit(), which derives the real guard value at boot (see stack-protector.h). */
#include "stack-protector.h"

#include "panic.h"

#include <arch/cpu.h>
#include <stdint.h>

uintptr_t __stack_chk_guard = 0xBADC0FFEE0DDF00DULL; /* replaced by stackGuardInit() at boot */

__attribute__((noreturn)) void __stack_chk_fail(void) {
    panic("stack protector: smashed");
}

static uint64_t readSeedWord(const uint8_t *seed, uint32_t wordIndex) {
    uint64_t w = 0;
    for (uint32_t b = 0; b < 8; b++) {
        w |= (uint64_t)seed[wordIndex * 8 + b] << (8 * b);
    }
    return w;
}

static uint64_t rotl64(uint64_t v, uint32_t bits) {
    return (v << bits) | (v >> (64 - bits));
}

/* splitmix64's finalizer (public-domain, Vigna): a cheap, well-mixed avalanche used only to fold
 * the seed material, not as a general-purpose PRNG (that's M2.6's ChaCha20 CSPRNG). */
static uint64_t splitmix64Finalize(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return z;
}

__attribute__((no_stack_protector, noinline)) bool stackGuardInit(const uint8_t seed[64]) {
    uint64_t acc = 0;
    bool seedAllZero = true;
    for (uint32_t i = 0; i < 8; i++) {
        uint64_t w = readSeedWord(seed, i);
        if (w != 0) {
            seedAllZero = false;
        }
        acc ^= w;
        acc = rotl64(acc, 23) * 0x9E3779B97F4A7C15ULL;
    }
    acc = splitmix64Finalize(acc);
    acc &= ~(uint64_t)0xFF; /* zero the low byte: a string-copy overflow can't reproduce it */

    bool ok = !seedAllZero && acc != 0;
    if (!ok) {
        uint64_t fallback = splitmix64Finalize(archReadTsc() ^ 0xA5A5A5A5A5A5A5A5ULL);
        fallback &= ~(uint64_t)0xFF;
        if (fallback == 0) {
            fallback = 0x100; /* never install a zero guard */
        }
        acc = fallback;
    }

    __stack_chk_guard = (uintptr_t)acc;
    return ok;
}
