/* ktests for the hardware random sources (kernel/arch/x86_64/hwrandom.c, M2.6 step 5, D-122):
 * archHwRandomSeed64/archHwRandom64 must deliver exactly when CPUID says the instruction exists.
 * Without this, a broken feature probe or carry-flag test silently degrades every boot to
 * "hw words 0/8 via none" and nothing else notices (the RNG still works on the loader seed). */
#include "ktest.h"

#include "../include/cpu-impl.h"

#include <arch/hwrandom.h>
#include <stdbool.h>
#include <stdint.h>

#define HWRANDOM_TEST_SENTINEL 0x5A5A5A5A5A5A5A5Aull

/* One source against its CPUID bit: when present, 4 calls all succeed with plausible, pairwise
 * different values (a real source repeats with probability ~2^-60 here); when absent, every call
 * fails and leaves `*out` untouched. */
static bool hwSourceMatches(bool (*source)(uint64_t *), bool present) {
    uint64_t v[4];
    for (int i = 0; i < 4; i++) {
        v[i] = HWRANDOM_TEST_SENTINEL;
        bool ok = source(&v[i]);
        if (ok != present) {
            return false;
        }
        if (!present) {
            if (v[i] != HWRANDOM_TEST_SENTINEL) {
                return false;
            }
            continue;
        }
        if (v[i] == 0 || v[i] == ~(uint64_t)0) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (v[i] == v[j]) {
                return false;
            }
        }
    }
    return true;
}

KTEST(hwrandom_matches_cpuid) {
    uint32_t r[4];
    archCpuid(0, 0, r);
    bool hasSeed = false;
    if (r[0] >= 7) {
        archCpuid(7, 0, r);
        hasSeed = (r[1] & (1u << 18)) != 0; /* CPUID.(EAX=07H,ECX=0):EBX.RDSEED[bit 18] */
    }
    archCpuid(1, 0, r);
    bool hasRand = (r[2] & (1u << 30)) != 0; /* CPUID.01H:ECX.RDRAND[bit 30] */

    KTEST_ASSERT(hwSourceMatches(archHwRandomSeed64, hasSeed));
    KTEST_ASSERT(hwSourceMatches(archHwRandom64, hasRand));
}
