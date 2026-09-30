/* Host tests for the kernel RNG core (kernel/core/random-core.c, M2.6 step 5): the ChaCha20
 * fast-key-erasure generator, the SHA-256 pool and the reseed step, on private RandomState
 * instances. The hardware sources and the lock live in random.c and are covered by the ktests. */
#include "crypto/chacha20.h"
#include "crypto/sha256.h"
#include "drbg-vectors.h"
#include "framework/test.h"
#include "random-core.h"

#include <stdlib.h>
#include <string.h>

static void setKeyTo0to31(RandomState *s) {
    randomCoreInit(s);
    for (int i = 0; i < 32; i++) {
        s->key[i] = DRBG_VECTOR_KEY_BYTE(i);
    }
}

/* Independent model of one generate step, straight from the RFC 8439 block function. */
static void refStep(uint8_t key[32], uint8_t *out, size_t m) {
    static const uint8_t zeroNonce[12] = {0};
    size_t blocks = (32 + m + 63) / 64;
    uint8_t *ks = malloc(blocks * 64);
    for (size_t b = 0; b < blocks; b++) {
        chacha20Block(key, (uint32_t)b, zeroNonce, ks + 64 * b);
    }
    memcpy(out, ks + 32, m);
    memcpy(key, ks, 32);
    free(ks);
}

static void refGenerate(uint8_t key[32], uint8_t *out, size_t n) {
    while (n > 0) {
        size_t m = n > 512 ? 512 : n;
        refStep(key, out, m);
        out += m;
        n -= m;
    }
}

TEST(drbgKnownAnswer64) {
    RandomState s;
    setKeyTo0to31(&s);
    uint8_t out[64];
    randomCoreGenerate(&s, out, sizeof(out));
    ASSERT_TRUE(memcmp(out, drbgVectorOut64, 64) == 0);
    ASSERT_TRUE(memcmp(s.key, drbgVectorNewKey, 32) == 0);
}

/* A 32-byte step uses ONE block: output = block0[32..64), newKey = block0[0..32). */
TEST(drbgKnownAnswer32MatchesChacha20Block) {
    static const uint8_t zeroNonce[12] = {0};
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        key[i] = DRBG_VECTOR_KEY_BYTE(i);
    }
    uint8_t blk[64];
    chacha20Block(key, 0, zeroNonce, blk);

    RandomState s;
    setKeyTo0to31(&s);
    uint8_t out[32];
    randomCoreGenerate(&s, out, sizeof(out));
    ASSERT_TRUE(memcmp(s.key, blk, 32) == 0);
    ASSERT_TRUE(memcmp(out, blk + 32, 32) == 0);
    ASSERT_TRUE(memcmp(out, drbgVectorOut64, 32) == 0);
    ASSERT_TRUE(memcmp(s.key, drbgVectorNewKey, 32) == 0);
}

/* The output never contains the new key: the two halves of block 0 are disjoint. */
TEST(drbgNewKeyIsNotPartOfTheOutput) {
    RandomState s;
    setKeyTo0to31(&s);
    uint8_t out[64];
    randomCoreGenerate(&s, out, sizeof(out));
    for (size_t i = 0; i + 32 <= sizeof(out); i++) {
        ASSERT_TRUE(memcmp(out + i, s.key, 32) != 0);
    }
}

/* Every step size around the 512-byte step boundary and the block boundaries, against the model,
 * and the output buffer is never written past `n`. */
TEST(drbgStepSizeBoundaries) {
    static const size_t sizes[] = {0,   1,   2,   31,  32,   33,   63,   64,   65,   95,
                                   96,  97,  127, 128, 129,  480,  481,  511,  512,  513,
                                   544, 545, 576, 577, 1023, 1024, 1025, 1536, 1537, 4096};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        RandomState s;
        setKeyTo0to31(&s);
        uint8_t refKey[32];
        memcpy(refKey, s.key, 32);

        uint8_t *got = malloc(n + 64);
        uint8_t *want = malloc(n + 64);
        ASSERT_TRUE(got != NULL && want != NULL);
        memset(got, 0xA5, n + 64);
        memset(want, 0xA5, n + 64);

        randomCoreGenerate(&s, got, n);
        refGenerate(refKey, want, n);

        int ok = memcmp(got, want, n + 64) == 0 && memcmp(s.key, refKey, 32) == 0;
        if (!ok) {
            fprintf(stderr, "  drbg mismatch at n=%zu\n", n);
        }
        free(got);
        free(want);
        ASSERT_TRUE(ok);
    }
}

/* n == 0 is a no-op: it must not step (and so not erase) the key. */
TEST(drbgZeroLengthDoesNotStep) {
    RandomState s;
    setKeyTo0to31(&s);
    uint8_t before[32];
    memcpy(before, s.key, 32);
    randomCoreGenerate(&s, NULL, 0);
    ASSERT_TRUE(memcmp(before, s.key, 32) == 0);
}

/* The key changes on every step, so two calls of the same size give different bytes, and 513
 * bytes in one call is NOT the same as 512 + 1 bytes in two calls' concatenation only if the steps
 * differ: it is exactly two steps (512, then 1), which is what the model says. */
TEST(drbgEveryStepErasesTheKey) {
    RandomState s;
    setKeyTo0to31(&s);
    uint8_t a[16], b[16];
    uint8_t k0[32], k1[32];
    memcpy(k0, s.key, 32);
    randomCoreGenerate(&s, a, sizeof(a));
    memcpy(k1, s.key, 32);
    randomCoreGenerate(&s, b, sizeof(b));
    ASSERT_TRUE(memcmp(k0, k1, 32) != 0);
    ASSERT_TRUE(memcmp(k1, s.key, 32) != 0);
    ASSERT_TRUE(memcmp(a, b, sizeof(a)) != 0);
}

TEST(drbgSplitCallsEqualSeparateSteps) {
    RandomState s;
    setKeyTo0to31(&s);
    uint8_t refKey[32];
    memcpy(refKey, s.key, 32);
    uint8_t got[100], want[100];
    randomCoreGenerate(&s, got, 40);
    randomCoreGenerate(&s, got + 40, 60);
    refStep(refKey, want, 40);
    refStep(refKey, want + 40, 60);
    ASSERT_TRUE(memcmp(got, want, sizeof(got)) == 0);
}

TEST(reseedKnownAnswer) {
    RandomState s;
    randomCoreInit(&s);
    randomCoreAddEntropy(&s, "abc", 3);
    randomCoreReseed(&s);
    ASSERT_TRUE(memcmp(s.key, drbgVectorReseedAbc, 32) == 0);
    ASSERT_EQ(s.generation, 1ull);
}

/* The reseed formula, recomputed with the public SHA-256 API for a nonzero key and generation
 * (the KAT above has both at zero, so it cannot catch a swapped or dropped term). */
TEST(reseedFormulaUsesOldKeyGenerationAndPoolDigest) {
    RandomState s;
    setKeyTo0to31(&s);
    s.generation = 0x0102030405060708ull;
    const uint8_t entropy[5] = {9, 8, 7, 6, 5};
    randomCoreAddEntropy(&s, entropy, sizeof(entropy));

    uint8_t oldKey[32];
    memcpy(oldKey, s.key, 32);
    uint8_t poolDigest[32];
    Sha256Ctx c;
    sha256Init(&c);
    sha256Update(&c, "rng-pool-v1", 11);
    sha256Update(&c, entropy, sizeof(entropy));
    sha256Final(&c, poolDigest);
    uint8_t gen[8] = {8, 7, 6, 5, 4, 3, 2, 1}; /* little endian */
    uint8_t want[32];
    sha256Init(&c);
    sha256Update(&c, "rng-reseed-v1", 13);
    sha256Update(&c, oldKey, 32);
    sha256Update(&c, gen, 8);
    sha256Update(&c, poolDigest, 32);
    sha256Final(&c, want);

    randomCoreReseed(&s);
    ASSERT_TRUE(memcmp(s.key, want, 32) == 0);
    ASSERT_EQ(s.generation, 0x0102030405060709ull);
}

TEST(reseedIsDeterministicAndSensitiveToEveryInput) {
    RandomState a, b;
    randomCoreInit(&a);
    randomCoreInit(&b);
    randomCoreAddEntropy(&a, "seed-one", 8);
    randomCoreAddEntropy(&b, "seed-one", 8);
    randomCoreReseed(&a);
    randomCoreReseed(&b);
    ASSERT_TRUE(memcmp(a.key, b.key, 32) == 0);

    /* Different entropy -> different key. */
    RandomState c;
    randomCoreInit(&c);
    randomCoreAddEntropy(&c, "seed-two", 8);
    randomCoreReseed(&c);
    ASSERT_TRUE(memcmp(a.key, c.key, 32) != 0);

    /* Same entropy split across calls = the same hash input. */
    RandomState d;
    randomCoreInit(&d);
    randomCoreAddEntropy(&d, "seed-", 5);
    randomCoreAddEntropy(&d, "one", 3);
    randomCoreReseed(&d);
    ASSERT_TRUE(memcmp(a.key, d.key, 32) == 0);

    /* A second reseed with the very same (empty) pool still changes the key: the old key and the
     * generation are part of the input. */
    uint8_t first[32];
    memcpy(first, a.key, 32);
    randomCoreReseed(&a);
    ASSERT_TRUE(memcmp(first, a.key, 32) != 0);

    /* The old key matters: same new entropy on top of different keys gives different keys. */
    RandomState e, f;
    randomCoreInit(&e);
    randomCoreInit(&f);
    e.key[0] = 1;
    randomCoreAddEntropy(&e, "x", 1);
    randomCoreAddEntropy(&f, "x", 1);
    randomCoreReseed(&e);
    randomCoreReseed(&f);
    ASSERT_TRUE(memcmp(e.key, f.key, 32) != 0);

    /* The generation matters: same key, same entropy, different generation. */
    RandomState g, h;
    randomCoreInit(&g);
    randomCoreInit(&h);
    h.generation = 7;
    randomCoreAddEntropy(&g, "x", 1);
    randomCoreAddEntropy(&h, "x", 1);
    randomCoreReseed(&g);
    randomCoreReseed(&h);
    ASSERT_TRUE(memcmp(g.key, h.key, 32) != 0);
}

/* The pool restarts on reseed: entropy absorbed before a reseed does not reach the next one. */
TEST(reseedRestartsThePool) {
    RandomState a, b;
    randomCoreInit(&a);
    randomCoreInit(&b);
    randomCoreAddEntropy(&a, "old", 3);
    randomCoreReseed(&a);
    randomCoreReseed(&b); /* b never saw "old" */
    ASSERT_TRUE(memcmp(a.key, b.key, 32) != 0);
    /* Both now at generation 1 with different keys; give them the same key and compare the next
     * reseed: only the pool could still differ, and it must not. */
    memcpy(b.key, a.key, 32);
    randomCoreAddEntropy(&a, "new", 3);
    randomCoreAddEntropy(&b, "new", 3);
    randomCoreReseed(&a);
    randomCoreReseed(&b);
    ASSERT_TRUE(memcmp(a.key, b.key, 32) == 0);
}

TEST(generationIncrementsPerReseed) {
    RandomState s;
    randomCoreInit(&s);
    ASSERT_EQ(s.generation, 0ull);
    for (uint64_t i = 1; i <= 5; i++) {
        randomCoreAddEntropy(&s, "e", 1);
        randomCoreReseed(&s);
        ASSERT_EQ(s.generation, i);
    }
    /* Generating bytes is not a reseed. */
    uint8_t out[8];
    randomCoreGenerate(&s, out, sizeof(out));
    ASSERT_EQ(s.generation, 5ull);
}

TEST(pendingAccounting) {
    RandomState s;
    randomCoreInit(&s);
    ASSERT_EQ(s.pending, 0u);
    randomCoreAddEntropy(&s, "abcde", 5);
    ASSERT_EQ(s.pending, 5u);
    randomCoreAddEntropy(&s, NULL, 0); /* no-op, NULL allowed */
    ASSERT_EQ(s.pending, 5u);
    uint8_t big[100] = {0};
    randomCoreAddEntropy(&s, big, sizeof(big));
    ASSERT_EQ(s.pending, 105u);
    /* Generating does not touch it. */
    uint8_t out[8];
    randomCoreGenerate(&s, out, sizeof(out));
    ASSERT_EQ(s.pending, 105u);
    randomCoreReseed(&s);
    ASSERT_EQ(s.pending, 0u);
    /* Saturates instead of wrapping. */
    s.pending = 0xFFFFFFF0u;
    randomCoreAddEntropy(&s, big, 100);
    ASSERT_EQ(s.pending, 0xFFFFFFFFu);
    randomCoreAddEntropy(&s, "x", 1);
    ASSERT_EQ(s.pending, 0xFFFFFFFFu);
}

TEST(initResetsEverything) {
    RandomState s;
    setKeyTo0to31(&s);
    s.generation = 9;
    s.pending = 9;
    randomCoreInit(&s);
    uint8_t zero[32] = {0};
    ASSERT_TRUE(memcmp(s.key, zero, 32) == 0);
    ASSERT_EQ(s.generation, 0ull);
    ASSERT_EQ(s.pending, 0u);
    /* The pool is a fresh "rng-pool-v1" context: same as the KAT's starting pool. */
    randomCoreAddEntropy(&s, "abc", 3);
    randomCoreReseed(&s);
    ASSERT_TRUE(memcmp(s.key, drbgVectorReseedAbc, 32) == 0);
}

/* randomCoreSeed (what randomInit runs on the global state): a known answer over a seed and three
 * words, computed with python hashlib as
 *   SHA-256("rng-reseed-v1" || 32 zero bytes || le64(0) ||
 *           SHA-256("rng-pool-v1" || seed || le64(w0) || le64(w1) || le64(w2))),
 * seed = 40 41 ... 7f, words = 0x0123456789abcdef, 0xfedcba9876543210, 1. */
static const uint8_t seedKatKey[32] = {
    0x2f, 0x4a, 0x4b, 0x1b, 0xdd, 0x6f, 0x37, 0x24, 0xfe, 0x6c, 0x23, 0x24, 0xbb, 0xb0, 0xcc, 0xb4,
    0x0e, 0xf3, 0x75, 0x86, 0x34, 0x02, 0xdf, 0xe1, 0xcc, 0x6e, 0x46, 0x67, 0x6c, 0x46, 0x4d, 0xa8,
};

static void seedKatInputs(uint8_t seed[RANDOM_SEED_SIZE], uint64_t words[3]) {
    for (int i = 0; i < RANDOM_SEED_SIZE; i++) {
        seed[i] = (uint8_t)(0x40 + i);
    }
    words[0] = 0x0123456789abcdefull;
    words[1] = 0xfedcba9876543210ull;
    words[2] = 1;
}

TEST(seedKnownAnswer) {
    uint8_t seed[RANDOM_SEED_SIZE];
    uint64_t words[3];
    seedKatInputs(seed, words);
    RandomState s;
    setKeyTo0to31(&s); /* stale state: randomCoreSeed must start from randomCoreInit */
    s.generation = 5;
    s.pending = 5;
    randomCoreAddEntropy(&s, "stale", 5);
    randomCoreSeed(&s, seed, words, 3);
    ASSERT_TRUE(memcmp(s.key, seedKatKey, 32) == 0);
    ASSERT_EQ(s.generation, 1ull);
    ASSERT_EQ(s.pending, 0u);
    /* randomCoreAddWord is exactly the 8 little-endian bytes. */
    RandomState a, b;
    randomCoreInit(&a);
    randomCoreInit(&b);
    const uint8_t le[8] = {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
    randomCoreAddWord(&a, 0x0123456789abcdefull);
    randomCoreAddEntropy(&b, le, sizeof(le));
    ASSERT_EQ(a.pending, 8u);
    randomCoreReseed(&a);
    randomCoreReseed(&b);
    ASSERT_TRUE(memcmp(a.key, b.key, 32) == 0);
}

/* Every seed byte, every word, and the word count reach the key (dropping or truncating any input
 * in randomCoreSeed fails here); count 0 with words == NULL is allowed. */
TEST(seedDependsOnEveryInput) {
    uint8_t seed[RANDOM_SEED_SIZE];
    uint64_t words[3];
    seedKatInputs(seed, words);
    RandomState s;
    for (int i = 0; i < RANDOM_SEED_SIZE; i++) {
        seed[i] ^= 0x01;
        randomCoreSeed(&s, seed, words, 3);
        seed[i] ^= 0x01;
        ASSERT_TRUE(memcmp(s.key, seedKatKey, 32) != 0);
    }
    for (int w = 0; w < 3; w++) {
        for (int bit = 0; bit < 64; bit += 9) {
            words[w] ^= 1ull << bit;
            randomCoreSeed(&s, seed, words, 3);
            words[w] ^= 1ull << bit;
            ASSERT_TRUE(memcmp(s.key, seedKatKey, 32) != 0);
        }
    }
    randomCoreSeed(&s, seed, words, 2);
    ASSERT_TRUE(memcmp(s.key, seedKatKey, 32) != 0);
    uint8_t noWords[32];
    randomCoreSeed(&s, seed, NULL, 0);
    memcpy(noWords, s.key, 32);
    ASSERT_EQ(s.generation, 1ull);
    ASSERT_TRUE(memcmp(noWords, seedKatKey, 32) != 0);
    randomCoreSeed(&s, seed, words, 3); /* and back: deterministic */
    ASSERT_TRUE(memcmp(s.key, seedKatKey, 32) == 0);
}
