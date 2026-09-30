/* The kernel random number generator (ARCHITECTURE §6.6, M2.6, D-122): an entropy pool
 * (SHA-256) feeding a ChaCha20 generator with fast key erasure. EXPERIMENTAL, unaudited crypto
 * (ARCHITECTURE §17). Every function here is IRQ-safe and never sleeps (the lock is IRQ-disable
 * only, like the pmm's, until M3.4 brings spinlocks), and none may be called before randomInit
 * except randomInit itself. Not reentrant: IRQ-disable keeps maskable interrupts out of a lock
 * holder, but an NMI, #MC or exception handler that interrupts one and calls in here would
 * corrupt the state, so none of them may call these functions (none does; panic and backtrace
 * never use the RNG). Buffers (`data`, `out`) must be mapped kernel memory: they are read/written
 * with the lock held, so a fault there would leave IRQs disabled (copy user buffers through a
 * kernel one). */
#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include <stddef.h>
#include <stdint.h>

/* Seeds the RNG exactly once, from kernelMain after stackGuardInit(). The pool absorbs the
 * loader's 64-byte `seed`, one TSC reading and up to 8 hardware words (RDSEED, each falling back
 * to RDRAND), and the first reseed turns that into the key. The seed is hashed, never used as key
 * or output directly, so no RNG output is a function of the seed that the KASLR slide and the
 * stack canary are also derived from in any way an observer of those can invert. The caller wipes
 * `seed` itself afterwards (cryptoWipe on the live BootInfo page, D-123). Logs
 * `random: seeded (hw words n/8 via RDSEED|RDRAND|none)` and, when n is 0, a WARN. Panics if
 * called twice. `noinline` so the hashing temporaries live in a frame that has returned before
 * anything else runs. Locks: takes the RNG lock. IRQ-safe: yes (but boot-time, BSP). May sleep:
 * no. */
void randomInit(const uint8_t seed[64]);

/* Absorbs `n` bytes of caller-supplied entropy (interrupt timing, device noise, ...) into the
 * pool; it only takes effect at the next reseed (randomGetBytes reseeds once 32 bytes are
 * pending). Nothing is credited or estimated, and it never blocks. Panics if called before
 * randomInit. `n` == 0 is a no-op. Locks: the RNG lock, held for at most 256 bytes at a time.
 * IRQ-safe: yes. May sleep: no. */
void randomAddEntropy(const void *data, size_t n);

/* Fills `out[0..n)` with cryptographically strong pseudo-random bytes (EXPERIMENTAL). Never
 * fails and never blocks: before the RNG is seeded it panics instead of returning weak bytes.
 * If at least 32 bytes of entropy are pending it reseeds first (stirring in 4 RDRAND words and the
 * TSC). Output is produced in steps of at most 512 bytes, each under the lock with IRQs off, so
 * a huge `n` does not hold IRQs off for long. `n` == 0 is a no-op (`out` may be NULL). Panics if
 * called before randomInit. Locks: the RNG lock, per step. IRQ-safe: yes. May sleep: no. */
void randomGetBytes(void *out, size_t n);

/* One pseudo-random 64-bit value (randomGetBytes of 8 bytes). Same contract. */
uint64_t randomU64(void);

/* Number of reseeds so far: 0 before randomInit, 1 right after it, +1 per reseed. For tests and
 * diagnostics; it reveals nothing about the key. Locks: the RNG lock. IRQ-safe: yes. May sleep:
 * no. */
uint64_t randomGeneration(void);

#endif
