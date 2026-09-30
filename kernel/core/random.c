/* Kernel RNG (ARCHITECTURE §6.6, D-122, D-123): the global instance of random-core.c's state, the
 * lock around it, and the hardware entropy sources. EXPERIMENTAL, unaudited crypto. */
#include "random.h"

#include "crypto/wipe.h"
#include "klog.h"
#include "panic.h"
#include "random-core.h"

#include <arch/cpu.h>
#include <arch/hwrandom.h>
#include <stdbool.h>

#define RANDOM_BOOT_HW_WORDS   8
#define RANDOM_RESEED_HW_WORDS 4
#define RANDOM_ADD_CHUNK       256 /* bytes hashed per lock hold in randomAddEntropy */

static RandomState rng;
static bool rngSeeded;

/* IRQ-disable-only lock (single CPU until M3.4/M3.5), the same shape as pmmLock(). */
static uint64_t randomLock(void) {
    return archIrqSave();
}
static void randomUnlock(uint64_t flags) {
    archIrqRestore(flags);
}

static void storeLe64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* Absorbs `v` (8 bytes, little endian) into the pool of `s`. */
static void poolAddWord(RandomState *s, uint64_t v) {
    uint8_t b[8];
    storeLe64(b, v);
    randomCoreAddEntropy(s, b, sizeof(b));
    cryptoWipe(b, sizeof(b));
}

__attribute__((noinline)) void randomInit(const uint8_t seed[64]) {
    uint64_t flags = randomLock();
    if (rngSeeded) {
        randomUnlock(flags);
        panic("random: randomInit() called twice");
    }

    randomCoreInit(&rng);
    /* The 64 loader bytes go into the pool through SHA-256: the key is a hash of them (plus more),
     * so no output is ever raw seed bytes. The same bytes also fed the KASLR slide (a splitmix64
     * hash, 8 bits kept, visible on serial) and the stack canary (D-077), so the RNG must not be
     * a cheap function of them -- SHA-256 makes the relation one-way. */
    randomCoreAddEntropy(&rng, seed, 64);
    poolAddWord(&rng, archReadTsc());

    uint32_t viaSeed = 0;
    uint32_t viaRand = 0;
    for (int i = 0; i < RANDOM_BOOT_HW_WORDS; i++) {
        uint64_t w;
        if (archHwRandomSeed64(&w)) {
            viaSeed++;
        } else if (archHwRandom64(&w)) {
            viaRand++;
        } else {
            continue;
        }
        poolAddWord(&rng, w);
        cryptoWipe(&w, sizeof(w));
    }

    randomCoreReseed(&rng);
    rngSeeded = true;
    randomUnlock(flags);

    uint32_t total = viaSeed + viaRand;
    const char *via = "none";
    if (viaSeed > 0 && viaRand > 0) {
        via = "RDSEED+RDRAND";
    } else if (viaSeed > 0) {
        via = "RDSEED";
    } else if (viaRand > 0) {
        via = "RDRAND";
    }
    klogWrite(KLOG_INFO, "random", "seeded (hw words %u/%u via %s)", total,
              (unsigned)RANDOM_BOOT_HW_WORDS, via);
    if (total == 0) {
        klogWrite(KLOG_WARN, "random",
                  "no hardware entropy source: seeded from the loader seed and the TSC only");
    }
}

void randomAddEntropy(const void *data, size_t n) {
    if (!rngSeeded) {
        panic("random: randomAddEntropy() before randomInit()");
    }
    const uint8_t *p = (const uint8_t *)data;
    while (n > 0) {
        size_t m = n > RANDOM_ADD_CHUNK ? RANDOM_ADD_CHUNK : n;
        uint64_t flags = randomLock();
        randomCoreAddEntropy(&rng, p, m);
        randomUnlock(flags);
        p += m;
        n -= m;
    }
}

/* Caller holds the lock. Stirs in fresh hardware words and the TSC, then reseeds. Hardware output
 * is only ever an additional input to the pool here (never the key itself), so a backdoored or
 * failing RDRAND cannot reduce what the pool already holds. */
static void reseedLocked(void) {
    for (int i = 0; i < RANDOM_RESEED_HW_WORDS; i++) {
        uint64_t w;
        if (archHwRandom64(&w)) {
            poolAddWord(&rng, w);
            cryptoWipe(&w, sizeof(w));
        }
    }
    poolAddWord(&rng, archReadTsc());
    randomCoreReseed(&rng);
}

void randomGetBytes(void *out, size_t n) {
    if (!rngSeeded) {
        panic("random: randomGetBytes() before randomInit()");
    }
    uint8_t *p = (uint8_t *)out;
    while (n > 0) {
        size_t m = n > RANDOM_STEP_MAX ? RANDOM_STEP_MAX : n;
        uint64_t flags = randomLock();
        if (rng.pending >= RANDOM_RESEED_PENDING) {
            reseedLocked();
        }
        randomCoreGenerate(&rng, p, m);
        randomUnlock(flags);
        p += m;
        n -= m;
    }
}

uint64_t randomU64(void) {
    uint8_t b[8];
    randomGetBytes(b, sizeof(b));
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | b[i];
    }
    cryptoWipe(b, sizeof(b));
    return v;
}

uint64_t randomGeneration(void) {
    uint64_t flags = randomLock();
    uint64_t g = rng.generation;
    randomUnlock(flags);
    return g;
}
