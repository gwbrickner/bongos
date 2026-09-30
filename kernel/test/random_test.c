/* ktests for the kernel RNG (M2.6 step 5, ARCHITECTURE §6.6, D-122): the generator's known
 * answers on a private state (same vectors as the host tests), the reseed machinery of the live
 * global RNG, and a cheap statistical sanity check of its output. Integers only -- the kernel
 * never uses the FPU. False-failure odds per boot for a truly random source (computed exactly
 * with a chi-square / normal tail, 2026-09-30 sweep): monobit and transitions are 10 standard
 * deviations wide (~1.5e-23 each); byte chi-square (255 df) below 100 is 3.8e-20, above 600 is
 * 7.5e-30; the two randomU64() values colliding is 2^-64 = 5.4e-20; so ~1e-19 in total. Constants,
 * counters, short cycles and other gross faults fail every time. */
#include "crypto/chacha20.h"
#include "drbg-vectors.h"
#include "kernel-boot.h"
#include "klog.h"
#include "ktest.h"
#include "random-core.h"
#include "random.h"
#include "vmalloc.h"

#include <arch/trap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool bytesEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void setKeyTo0to31(RandomState *s) {
    randomCoreInit(s);
    for (int i = 0; i < 32; i++) {
        s->key[i] = DRBG_VECTOR_KEY_BYTE(i);
    }
}

/* The live RNG must be seeded from this boot's entropy, not a constant: every other test here would
 * pass against an RNG that ignores its inputs (ChaCha20 output looks random under any key, and the
 * global key is private). So the first output of the key randomInit derived is printed, and
 * mk/test.mk's _check-ktest-pass requires the value to differ between every matrix boot (both
 * firmwares; a false failure needs a 64-bit collision). Must run before anything reseeds the
 * global RNG (reseeds stir in fresh RDRAND/TSC words, which would hide a constant boot seeding),
 * hence the first test in this file and the generation checks. Printing it is harmless: the value
 * is discarded, and fast key erasure replaces the key before randomU64 returns. */
KTEST(random_boot_unique) {
    KTEST_ASSERT_EQ(randomGeneration(), 1); /* only randomInit's reseed so far */
    uint64_t v = randomU64();
    KTEST_ASSERT_EQ(randomGeneration(), 1);
    klogWrite(KLOG_INFO, "random", "ktest boot fingerprint 0x%016llx", (unsigned long long)v);
}

/* ChaCha20 with fast key erasure: known answers, the step structure, and that the key really is
 * replaced after every step. */
KTEST(random_drbg_fast_key_erasure) {
    static const uint8_t zeroNonce[CHACHA20_NONCE_SIZE] = {0};
    RandomState s;

    /* KAT: a 64-byte step under key 00..1f. */
    setKeyTo0to31(&s);
    uint8_t out[64];
    randomCoreGenerate(&s, out, sizeof(out));
    KTEST_ASSERT(bytesEqual(out, drbgVectorOut64, sizeof(out)));
    KTEST_ASSERT(bytesEqual(s.key, drbgVectorNewKey, sizeof(s.key)));

    /* newKey == block0[0..32) and output == block0[32..64) for a 32-byte step, straight from the
     * RFC 8439 block function. */
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        key[i] = DRBG_VECTOR_KEY_BYTE(i);
    }
    uint8_t blk[CHACHA20_BLOCK_SIZE];
    chacha20Block(key, 0, zeroNonce, blk);
    setKeyTo0to31(&s);
    uint8_t out32[32];
    randomCoreGenerate(&s, out32, sizeof(out32));
    KTEST_ASSERT(bytesEqual(s.key, blk, 32));
    KTEST_ASSERT(bytesEqual(out32, blk + 32, 32));

    /* A multi-block, multi-step request (1100 bytes = steps of 512, 512, 76) against a model that
     * chains chacha20Block by hand under the evolving key. Checks sentinels past the end too. */
    static uint8_t got[1100 + 16];
    static uint8_t want[1100 + 16];
    for (size_t i = 0; i < sizeof(got); i++) {
        got[i] = 0xA5;
        want[i] = 0xA5;
    }
    setKeyTo0to31(&s);
    randomCoreGenerate(&s, got, 1100);
    size_t done = 0;
    while (done < 1100) {
        size_t m = 1100 - done > RANDOM_STEP_MAX ? RANDOM_STEP_MAX : 1100 - done;
        uint8_t ks[9 * CHACHA20_BLOCK_SIZE];
        size_t blocks = (32 + m + CHACHA20_BLOCK_SIZE - 1) / CHACHA20_BLOCK_SIZE;
        for (size_t b = 0; b < blocks; b++) {
            chacha20Block(key, (uint32_t)b, zeroNonce, ks + b * CHACHA20_BLOCK_SIZE);
        }
        for (size_t i = 0; i < m; i++) {
            want[done + i] = ks[32 + i];
        }
        for (int i = 0; i < 32; i++) {
            key[i] = ks[i];
        }
        done += m;
    }
    KTEST_ASSERT(bytesEqual(got, want, sizeof(got)));
    KTEST_ASSERT(bytesEqual(s.key, key, 32));

    /* n == 0 does not step; every real step replaces the key. */
    uint8_t before[32];
    for (int i = 0; i < 32; i++) {
        before[i] = s.key[i];
    }
    randomCoreGenerate(&s, out, 0);
    KTEST_ASSERT(bytesEqual(before, s.key, 32));
    randomCoreGenerate(&s, out, 1);
    KTEST_ASSERT(!bytesEqual(before, s.key, 32));
}

typedef struct {
    RandomState *s;
    uint8_t *out;
    size_t n;
} RandomGenerateArgs;

static void randomGenerateTrigger(void *arg) {
    RandomGenerateArgs *a = (RandomGenerateArgs *)arg;
    randomCoreGenerate(a->s, a->out, a->n);
}

/* Fast key erasure means the key is gone before any output leaves the generator: a step whose
 * output copy faults part-way (here: the caller's buffer runs into a vmalloc guard page, caught
 * with archTrapCatch) must already have replaced the key, or the next call would hand out the very
 * bytes the first caller already received. */
KTEST(random_drbg_key_erased_before_output) {
    uint8_t *page = (uint8_t *)vmalloc(4096, 0);
    KTEST_ASSERT(page != NULL);
    RandomState s;
    setKeyTo0to31(&s);
    RandomGenerateArgs args = {&s, page + 4096 - 16, 64}; /* 16 bytes fit, byte 17 faults */
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_VEC(14), randomGenerateTrigger, &args, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.cr2, (uint64_t)(uintptr_t)(page + 4096));
    KTEST_ASSERT(bytesEqual(page + 4096 - 16, drbgVectorOut64, 16)); /* what the caller got */
    KTEST_ASSERT(bytesEqual(s.key, drbgVectorNewKey, sizeof(s.key)));

    /* And so the retry is fresh output, not a replay of those 16 bytes. */
    uint8_t again[16];
    randomCoreGenerate(&s, again, sizeof(again));
    KTEST_ASSERT(!bytesEqual(again, drbgVectorOut64, sizeof(again)));
    vfree(page);
}

/* Entropy added to the live RNG is picked up by a reseed: randomGeneration() moves exactly when
 * at least 32 bytes are pending and randomGetBytes runs. The reseed's effect on the key is pinned
 * on a private state (the global key is not observable): a known answer, and the key must change
 * on every reseed even with nothing new in the pool. */
KTEST(random_add_entropy_reseeds) {
    uint8_t out[16];
    uint8_t entropy[32];
    for (size_t i = 0; i < sizeof(entropy); i++) {
        entropy[i] = (uint8_t)(0x40 + 7 * i);
    }

    KTEST_ASSERT(randomGeneration() >= 1); /* randomInit() ran and reseeded once */
    randomGetBytes(out, sizeof(out));      /* flush anything already pending */
    uint64_t g0 = randomGeneration();

    randomGetBytes(out, sizeof(out));
    KTEST_ASSERT_EQ(randomGeneration(), g0); /* nothing pending: no reseed */

    randomAddEntropy(entropy, 31);
    randomGetBytes(out, sizeof(out));
    KTEST_ASSERT_EQ(randomGeneration(), g0); /* 31 < 32 pending: not yet */

    randomAddEntropy(entropy, 1);
    randomGetBytes(out, sizeof(out));
    KTEST_ASSERT_EQ(randomGeneration(), g0 + 1); /* 32 pending: reseeded before generating */

    randomGetBytes(out, sizeof(out));
    KTEST_ASSERT_EQ(randomGeneration(), g0 + 1); /* pending was reset to 0 */

    randomAddEntropy(entropy, sizeof(entropy));
    randomGetBytes(out, sizeof(out));
    KTEST_ASSERT_EQ(randomGeneration(), g0 + 2);

    /* Private state: the reseed KAT ("abc"), then a second reseed on an empty pool must differ. */
    RandomState s;
    randomCoreInit(&s);
    randomCoreAddEntropy(&s, "abc", 3);
    KTEST_ASSERT_EQ(s.pending, 3);
    randomCoreReseed(&s);
    KTEST_ASSERT(bytesEqual(s.key, drbgVectorReseedAbc, sizeof(s.key)));
    KTEST_ASSERT_EQ(s.generation, 1);
    KTEST_ASSERT_EQ(s.pending, 0);
    uint8_t first[32];
    for (int i = 0; i < 32; i++) {
        first[i] = s.key[i];
    }
    randomCoreReseed(&s);
    KTEST_ASSERT(!bytesEqual(first, s.key, sizeof(first)));
    KTEST_ASSERT_EQ(s.generation, 2);
}

/* 64 KiB from the live RNG: monobit, bit transitions and a byte chi-square, all in integers, plus
 * "consecutive 64-byte outputs differ". The requests are 1300 bytes (steps of 512, 512 and 276
 * inside randomGetBytes; the last one 536) into a buffer zeroed first, so a step randomGetBytes
 * never writes shows up as a run of zero bytes, and a sentinel after each request catches an
 * overrun (M2.6 finish sweep: with 256-byte requests, randomGetBytes' multi-step loop was never
 * run by any test, and a loop that stopped advancing `p` passed). */
KTEST(random_sanity) {
    enum { TOTAL = 65536, CHUNK = 1300, TAIL = 16 };
    static uint8_t buf[CHUNK + TAIL];
    uint32_t hist[256];
    for (int i = 0; i < 256; i++) {
        hist[i] = 0;
    }
    int64_t ones = 0;
    int64_t transitions = 0;
    int prevBit = -1;

    for (size_t got = 0; got < TOTAL;) {
        size_t req = TOTAL - got < CHUNK ? TOTAL - got : CHUNK;
        for (size_t i = 0; i < sizeof(buf); i++) {
            buf[i] = i < req ? 0x00 : 0xA5;
        }
        randomGetBytes(buf, req);
        for (size_t i = req; i < sizeof(buf); i++) {
            KTEST_ASSERT_EQ(buf[i], 0xA5); /* nothing written past the request */
        }
        got += req;
        for (size_t i = 0; i < req; i++) {
            uint8_t b = buf[i];
            hist[b]++;
            for (int bit = 0; bit < 8; bit++) {
                int v = (b >> bit) & 1;
                ones += v;
                if (prevBit >= 0 && v != prevBit) {
                    transitions++;
                }
                prevBit = v;
            }
        }
    }
    int64_t totalBits = (int64_t)TOTAL * 8; /* 524288 */
    int64_t monoDev = ones - totalBits / 2;
    if (monoDev < 0) {
        monoDev = -monoDev;
    }
    KTEST_ASSERT(monoDev <= 3620);

    int64_t transDev = transitions - (totalBits - 1) / 2; /* 262143 */
    if (transDev < 0) {
        transDev = -transDev;
    }
    KTEST_ASSERT(transDev <= 3620);

    int64_t chi = 0;
    for (int i = 0; i < 256; i++) {
        int64_t d = (int64_t)hist[i] - 256;
        chi += d * d;
    }
    KTEST_ASSERT(chi > 100 * 256);
    KTEST_ASSERT(chi < 600 * 256);

    uint8_t prev[64];
    randomGetBytes(prev, sizeof(prev));
    for (int round = 0; round < 32; round++) {
        uint8_t cur[64];
        randomGetBytes(cur, sizeof(cur));
        KTEST_ASSERT(!bytesEqual(prev, cur, sizeof(cur)));
        for (size_t i = 0; i < sizeof(cur); i++) {
            prev[i] = cur[i];
        }
    }
    KTEST_ASSERT(randomU64() != randomU64());
}

/* D-123 seed lifecycle: kernelMain wipes the loader's live seed right after randomInit (the page
 * is also zeroed by the reclaim later, which is why kernelMain records a read-back for this test),
 * and kernelBootInfo()'s copy never holds seed bytes. A loader seed is never all zero (it always
 * mixes in at least TSC jitter), so a missing wipe shows up here. */
KTEST(random_boot_seed_wiped) {
    KTEST_ASSERT_EQ(kernelBootSeedResidue(), 0);
    const BootInfo *bi = kernelBootInfo();
    uint8_t acc = 0;
    for (size_t i = 0; i < sizeof(bi->randomSeed); i++) {
        acc |= bi->randomSeed[i];
    }
    KTEST_ASSERT_EQ(acc, 0);
    KTEST_ASSERT(randomGeneration() >= 1); /* the seed was consumed before it was wiped */
}
