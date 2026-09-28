/* See loader-cpu.h. Extracted from boot/uefi/handoff.c (D-104). */
#include "loader-cpu.h"

bool loaderCpuCheckLongModeFeatures(bool *outHas1G) {
    *outHas1G = false;

    uint32_t maxExtLeaf, ebx0, ecx0, edx0;
    __asm__ volatile("cpuid"
                     : "=a"(maxExtLeaf), "=b"(ebx0), "=c"(ecx0), "=d"(edx0)
                     : "a"(0x80000000));
    (void)ebx0;
    (void)ecx0;
    (void)edx0;
    /* CPUID.80000000h:EAX is the highest supported *extended* leaf (mirrors maxBasicLeaf() below
     * for leaf 0): querying 80000001h without this check first is unsafe on a CPU that doesn't
     * support extended leaves at all -- D-103/D-111 need to trust this before ever trying to set
     * EFER.LME, since that WRMSR raises #GP on a CPU without long mode. Only ever an issue on
     * BIOS, which has no firmware-level equivalent of "you're already an x86_64 UEFI application"
     * to lean on the way the UEFI loader implicitly does. */
    if (maxExtLeaf < 0x80000001u) {
        return false;
    }

    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    (void)eax;
    (void)ebx;
    (void)ecx;
    if ((edx & (1u << 29)) == 0) { /* LM (long mode) */
        return false;
    }
    *outHas1G = (edx & (1u << 26)) != 0;
    return (edx & (1u << 20)) != 0;
}

bool loaderCpuLa57Enabled(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return (cr4 & (1ull << 12)) != 0;
}

void loaderCpuSetEferNxe(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11);
    __asm__ volatile("wrmsr" : : "c"(0xC0000080u), "a"(lo), "d"(hi));
}

uint64_t loaderCpuRdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool rdseed32(uint32_t *out) {
    uint8_t ok;
    __asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(*out), "=qm"(ok));
    return ok != 0;
}

static bool rdrand32(uint32_t *out) {
    uint8_t ok;
    __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(*out), "=qm"(ok));
    return ok != 0;
}

/* CPUID.0:EAX is the highest supported *basic* leaf; querying leaf 7 without checking this first
 * is unsafe -- on Intel CPUs an out-of-range basic leaf request returns the highest supported
 * leaf's data instead of zeros, so a machine with a basic-leaf max below 7 would read garbage
 * into EBX and could spuriously believe RDSEED exists. */
static uint32_t maxBasicLeaf(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    return eax;
}

bool loaderCpuHasRdseed(void) {
    if (maxBasicLeaf() < 7) {
        return false;
    }
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    return (ebx & (1u << 18)) != 0;
}

bool loaderCpuHasRdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ecx & (1u << 30)) != 0;
}

static uint32_t drawHalf(bool hasRdseed, bool hasRdrand) {
    uint32_t v = 0;
    bool ok = false;
    if (hasRdseed) {
        for (int retry = 0; retry < 10 && !ok; retry++) {
            ok = rdseed32(&v);
        }
    }
    if (!ok && hasRdrand) {
        for (int retry = 0; retry < 10 && !ok; retry++) {
            ok = rdrand32(&v);
        }
    }
    return ok ? v : 0;
}

void loaderCpuRandomFill(uint64_t seed[8]) {
    bool hasRdseed = loaderCpuHasRdseed();
    bool hasRdrand = loaderCpuHasRdrand();
    for (int i = 0; i < 8; i++) {
        uint32_t lo = drawHalf(hasRdseed, hasRdrand);
        uint32_t hi = drawHalf(hasRdseed, hasRdrand);
        seed[i] = ((uint64_t)hi << 32) | lo;
    }
    uint64_t tsc = loaderCpuRdtsc();
    for (int i = 0; i < 8; i++) {
        seed[i] ^= tsc;
        tsc = tsc * 6364136223846793005ULL + 1; /* cheap avalanche between qwords */
    }
}

#define PAT_MSR 0x277u
bool loaderCpuPatEntry2Uncacheable(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(PAT_MSR));
    uint64_t pat = ((uint64_t)hi << 32) | lo;
    uint8_t entry2 = (uint8_t)((pat >> 16) & 0xFFu);
    return entry2 == 0x07u /* UC- */ || entry2 == 0x00u /* UC */;
}
