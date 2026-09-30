/* Portable-facing declarations for the CPU's hardware random sources (ARCHITECTURE §6.6, D-122).
 * The implementation is x86-specific (RDSEED/RDRAND, found via CPUID) and lives in
 * kernel/arch/x86_64/hwrandom.c, so kernel/core never touches cpuid or assembly. Both functions
 * are sources of *entropy to be mixed into the pool*, never a replacement for it: hardware output
 * is not trusted on its own (the RNG hashes it together with the loader seed and the TSC). */
#ifndef KERNEL_INCLUDE_ARCH_HWRANDOM_H
#define KERNEL_INCLUDE_ARCH_HWRANDOM_H

#include <stdbool.h>
#include <stdint.h>

/* Stores one 64-bit value from the CPU's hardware entropy source (x86: RDSEED, CPUID.07H:EBX[18])
 * in `*out` and returns true. Retries up to 64 times with `pause` while the instruction reports
 * "no data yet" (CF=0). A result of all-zero or all-ones counts as a failed try (a stuck or
 * broken source); after 64 failed tries it returns false. Returns false at once, leaving `*out`
 * untouched, when the CPU has no such instruction (unsupported is not an error: callers fall
 * back to archHwRandom64 or to other entropy). `out` must not be NULL. On any false return the
 * caller must not use `*out`. No locks; IRQ-safe (no shared state beyond a cached feature flag);
 * never sleeps; bounded time (at most 64 RDSEEDs). */
bool archHwRandomSeed64(uint64_t *out);

/* Same contract for the CPU's hardware DRBG (x86: RDRAND, CPUID.01H:ECX[30]) with 10 tries (the
 * number the Intel DRNG guide recommends). RDRAND output is a DRBG stream and is weaker evidence
 * of fresh entropy than RDSEED; the RNG uses it only as a fallback and for reseed stir-in. */
bool archHwRandom64(uint64_t *out);

#endif
