/* ktests for the kernel RNG (M2.6 step 5, ARCHITECTURE §6.6, D-122): the generator's known
 * answers on a private state (same vectors as the host tests), the reseed machinery of the live
 * global RNG, and a cheap statistical sanity check of its output. Integers only -- the kernel
 * never uses the FPU. The sanity thresholds are ten standard deviations wide, so a healthy RNG
 * fails with probability ~1e-23 per boot, while constants, counters, short cycles and other gross
 * faults fail every time. */
#include "crypto/chacha20.h"
#include "drbg-vectors.h"
#include "ktest.h"
#include "random-core.h"
#include "random.h"

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

/* 64 KiB from the live RNG in 256-byte chunks: monobit, bit transitions and a byte chi-square,
 * all in integers, plus "consecutive 64-byte outputs differ". */
KTEST(random_sanity) {
    enum { TOTAL = 65536, CHUNK = 256 };
    uint8_t buf[CHUNK];
    uint32_t hist[256];
    for (int i = 0; i < 256; i++) {
        hist[i] = 0;
    }
    int64_t ones = 0;
    int64_t transitions = 0;
    int prevBit = -1;

    for (size_t got = 0; got < TOTAL; got += CHUNK) {
        randomGetBytes(buf, CHUNK);
        for (size_t i = 0; i < CHUNK; i++) {
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
