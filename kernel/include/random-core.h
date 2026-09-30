/* The kernel RNG's pure core (ARCHITECTURE §6.6, D-122): an entropy pool (running SHA-256), a
 * ChaCha20 generator with fast key erasure, and the reseed step that joins them. No locks, no
 * hardware, no klog/panic: kernel/core/random.c owns the global instance, the lock and the
 * hardware sources, and tests/host and the ktests drive this file on private instances.
 * EXPERIMENTAL, unaudited crypto (ARCHITECTURE §17). */
#ifndef KERNEL_RANDOM_CORE_H
#define KERNEL_RANDOM_CORE_H

#include "crypto/chacha20.h"
#include "crypto/sha256.h"

#include <stddef.h>
#include <stdint.h>

/* Domain-separation labels (hashed, never printed). */
#define RANDOM_POOL_DOMAIN   "rng-pool-v1"
#define RANDOM_RESEED_DOMAIN "rng-reseed-v1"

/* A generate step computes at most this many output bytes under one key before that key is
 * erased (longer requests are split into steps, each under its own fresh key). */
#define RANDOM_STEP_MAX 512

/* randomGetBytes reseeds first when at least this many bytes were added since the last reseed. */
#define RANDOM_RESEED_PENDING 32

typedef struct RandomState {
    Sha256Ctx pool;      /* running hash of everything added since the last reseed */
    uint64_t generation; /* number of reseeds so far; 0 = never seeded */
    uint32_t pending;    /* bytes added to the pool since the last reseed (saturating) */
    uint8_t key[CHACHA20_KEY_SIZE];
} RandomState;

/* Resets `s` to the unseeded state: all-zero key, generation 0, pending 0, and a pool that has
 * absorbed only RANDOM_POOL_DOMAIN. The key is useless until the first randomCoreReseed.
 * No locks, may not sleep, IRQ-safe, cannot fail. */
void randomCoreInit(RandomState *s);

/* Absorbs `n` bytes into the pool (no crediting, no estimation: every byte only adds to
 * `pending`, which saturates at UINT32_MAX) and never blocks. `n` == 0 is a no-op and `data` may
 * then be NULL. `s` must have been through randomCoreInit. No locks, may not sleep, IRQ-safe,
 * cannot fail. */
void randomCoreAddEntropy(RandomState *s, const void *data, size_t n);

/* Reseeds: d = SHA-256(pool); key = SHA-256("rng-reseed-v1" || oldKey || le64(generation) || d);
 * generation++; pending = 0; the pool restarts (domain only). Hashing the OLD key in makes the new
 * key depend on the whole history, so a reseed with guessable entropy never weakens an unknown
 * key. All temporaries (digest, new key) are wiped. No locks, may not sleep, IRQ-safe, cannot
 * fail. */
void randomCoreReseed(RandomState *s);

/* Writes `n` pseudo-random bytes to `out` using fast key erasure. Each step of up to
 * RANDOM_STEP_MAX bytes computes ceil((32 + m) / 64) ChaCha20 blocks (counters 0.., zero nonce)
 * under the current key FIRST; bytes [0, 32) of the keystream become the next key and the m bytes
 * after them are the output; only then is the key replaced and the keystream wiped. So a later
 * compromise of the state reveals nothing about earlier output. `n` == 0 writes nothing and does
 * not step. The state must have been seeded (generation > 0) -- the core does not check; random.c
 * does. `out` must not overlap `s`. No locks, may not sleep, IRQ-safe, cannot fail. */
void randomCoreGenerate(RandomState *s, void *out, size_t n);

#endif
