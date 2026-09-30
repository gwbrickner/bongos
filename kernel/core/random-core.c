/* See kernel/include/random-core.h. Pure: no hardware, locks, klog or panic. Code rules for
 * secret data (ARCHITECTURE §17): no secret-dependent branch or index, every temporary that held
 * key material or keystream is wiped before return. */
#include "random-core.h"

#include "crypto/wipe.h"

#define KEYSTREAM_MAX_BLOCKS                                                                       \
    ((CHACHA20_KEY_SIZE + RANDOM_STEP_MAX + CHACHA20_BLOCK_SIZE - 1) / CHACHA20_BLOCK_SIZE)

static void copyBytes(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void poolRestart(RandomState *s) {
    sha256Init(&s->pool);
    sha256Update(&s->pool, RANDOM_POOL_DOMAIN, sizeof(RANDOM_POOL_DOMAIN) - 1);
}

void randomCoreInit(RandomState *s) {
    cryptoWipe(s->key, sizeof(s->key));
    s->generation = 0;
    s->pending = 0;
    poolRestart(s);
}

void randomCoreAddEntropy(RandomState *s, const void *data, size_t n) {
    if (n == 0) {
        return;
    }
    sha256Update(&s->pool, data, n);
    uint64_t total = (uint64_t)s->pending + (uint64_t)n;
    s->pending = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

void randomCoreReseed(RandomState *s) {
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t newKey[CHACHA20_KEY_SIZE];
    uint8_t gen[8];
    sha256Final(&s->pool, digest); /* also wipes the pool context */

    for (int i = 0; i < 8; i++) {
        gen[i] = (uint8_t)(s->generation >> (8 * i));
    }
    Sha256Ctx h;
    sha256Init(&h);
    sha256Update(&h, RANDOM_RESEED_DOMAIN, sizeof(RANDOM_RESEED_DOMAIN) - 1);
    sha256Update(&h, s->key, sizeof(s->key));
    sha256Update(&h, gen, sizeof(gen));
    sha256Update(&h, digest, sizeof(digest));
    sha256Final(&h, newKey); /* wipes h */

    copyBytes(s->key, newKey, sizeof(newKey));
    s->generation++;
    s->pending = 0;
    poolRestart(s);

    cryptoWipe(digest, sizeof(digest));
    cryptoWipe(newKey, sizeof(newKey));
    cryptoWipe(gen, sizeof(gen));
}

/* One fast-key-erasure step for m <= RANDOM_STEP_MAX output bytes. */
static void generateStep(RandomState *s, uint8_t *out, size_t m) {
    static const uint8_t zeroNonce[CHACHA20_NONCE_SIZE] = {0};
    uint8_t ks[KEYSTREAM_MAX_BLOCKS * CHACHA20_BLOCK_SIZE];
    size_t blocks = (CHACHA20_KEY_SIZE + m + CHACHA20_BLOCK_SIZE - 1) / CHACHA20_BLOCK_SIZE;

    /* Everything is computed under the OLD key before it is replaced. */
    for (size_t b = 0; b < blocks; b++) {
        chacha20Block(s->key, (uint32_t)b, zeroNonce, ks + b * CHACHA20_BLOCK_SIZE);
    }
    copyBytes(out, ks + CHACHA20_KEY_SIZE, m);
    copyBytes(s->key, ks, CHACHA20_KEY_SIZE);
    cryptoWipe(ks, sizeof(ks));
}

void randomCoreGenerate(RandomState *s, void *out, size_t n) {
    uint8_t *p = (uint8_t *)out;
    while (n > 0) {
        size_t m = n > RANDOM_STEP_MAX ? RANDOM_STEP_MAX : n;
        generateStep(s, p, m);
        p += m;
        n -= m;
    }
}
