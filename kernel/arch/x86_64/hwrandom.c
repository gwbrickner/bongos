/* See kernel/include/arch/hwrandom.h. RDSEED / RDRAND (SDM Vol 1 §7.3.17). The instructions are
 * emitted as inline assembly so the file needs no -mrdseed/-mrdrnd flags, and they are only ever
 * executed after the CPUID feature bit said they exist (they are #UD otherwise). */
#include <arch/hwrandom.h>

#include "include/cpu-impl.h"

#include <stdbool.h>
#include <stdint.h>

#define HWRANDOM_SEED_TRIES 64
#define HWRANDOM_RAND_TRIES 10

/* Cached CPUID results: 0 = not probed yet, 1 = absent, 2 = present. CPUID is slow (a VM exit
 * under virtualization) and the answer never changes; a racing first probe just stores the same
 * value twice. */
static uint8_t rdseedState;
static uint8_t rdrandState;

/* CPUID.(EAX=07H,ECX=0):EBX[18], guarded by the maximum basic leaf so a CPU without leaf 7 is
 * not asked a question it may answer with garbage. */
static bool cpuHasRdseed(void) {
    if (rdseedState == 0) {
        uint32_t r[4];
        archCpuid(0, 0, r);
        bool has = false;
        if (r[0] >= 7) {
            archCpuid(7, 0, r);
            has = ((r[1] >> 18) & 1) != 0;
        }
        rdseedState = has ? 2 : 1;
    }
    return rdseedState == 2;
}

/* CPUID.01H:ECX[30]. */
static bool cpuHasRdrand(void) {
    if (rdrandState == 0) {
        uint32_t r[4];
        archCpuid(1, 0, r);
        rdrandState = ((r[2] >> 30) & 1) != 0 ? 2 : 1;
    }
    return rdrandState == 2;
}

/* One RDSEED: true with `*out` set iff CF=1 (a value was delivered). */
static bool rdseedOnce(uint64_t *out) {
    uint64_t v;
    uint8_t ok;
    __asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(v), "=qm"(ok) : : "cc");
    *out = v;
    return ok != 0;
}

static bool rdrandOnce(uint64_t *out) {
    uint64_t v;
    uint8_t ok;
    __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(v), "=qm"(ok) : : "cc");
    *out = v;
    return ok != 0;
}

/* A stuck or failing source tends to return all-zero or all-ones; real output has probability
 * 2^-63 of being either. */
static bool plausibleWord(uint64_t v) {
    return v != 0 && v != ~(uint64_t)0;
}

bool archHwRandomSeed64(uint64_t *out) {
    if (!cpuHasRdseed()) {
        return false;
    }
    for (int i = 0; i < HWRANDOM_SEED_TRIES; i++) {
        uint64_t v;
        if (rdseedOnce(&v) && plausibleWord(v)) {
            *out = v;
            return true;
        }
        archPause();
    }
    return false;
}

bool archHwRandom64(uint64_t *out) {
    if (!cpuHasRdrand()) {
        return false;
    }
    for (int i = 0; i < HWRANDOM_RAND_TRIES; i++) {
        uint64_t v;
        if (rdrandOnce(&v) && plausibleWord(v)) {
            *out = v;
            return true;
        }
        archPause();
    }
    return false;
}
