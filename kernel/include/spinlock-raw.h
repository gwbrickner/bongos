/* The ticket-lock core with no validator hooks and no preemption accounting (M3.4, D-183). Header
 * only and pure (it needs only atomic.h and archPause()), so the host tests exercise exactly the
 * code the kernel runs.
 *
 * A raw lock is neither validated nor does it touch preemptCount or the interrupt flag: the
 * caller disables IRQs first. It exists for the lock validator's own graph lock (which must not
 * recurse into the validator) and, later, NMI-reachable code. Everything else uses Spinlock
 * (spinlock.h). */
#ifndef KERNEL_SPINLOCK_RAW_H
#define KERNEL_SPINLOCK_RAW_H

#include "atomic.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stdint.h>

/* Ticket lock: free iff next == owner. Two full 32-bit counters (not packed halves), so a trylock
 * needs one compare-exchange and wrap-around is harmless (differences are taken mod 2^32). */
typedef struct RawSpinlock {
    uint32_t next;  /* next ticket to hand out */
    uint32_t owner; /* ticket now being served */
} RawSpinlock;

#define RAW_SPINLOCK_INIT                                                                          \
    { 0, 0 }

/* Takes a ticket and spins (PAUSE) until it is served: FIFO-fair. Acquire ordering on success.
 * May spin forever if the holder never releases. Caller disables IRQs when the lock is also taken
 * from a handler. Any context. */
static inline void rawSpinLock(RawSpinlock *l) {
    uint32_t ticket = ATOMIC_FETCH_ADD(&l->next, 1u, MEM_RELAXED);
    while (ATOMIC_LOAD(&l->owner, MEM_ACQUIRE) != ticket) {
        archPause();
    }
}

/* Takes the lock only if it is free right now (no ticket is consumed on failure). Acquire
 * ordering on success. Never spins. Any context. */
static inline bool rawSpinTryLock(RawSpinlock *l) {
    uint32_t o = ATOMIC_LOAD(&l->owner, MEM_RELAXED);
    uint32_t expected = o;
    /* Succeeds only if next == owner (nobody holds or waits), claiming the ticket `o`. */
    return ATOMIC_CMPXCHG(&l->next, &expected, o + 1u, MEM_ACQUIRE, MEM_RELAXED);
}

/* Releases the lock: serves the next ticket. The caller must hold it (not checked here; spinlock.c
 * checks). Release ordering. Any context. */
static inline void rawSpinUnlock(RawSpinlock *l) {
    ATOMIC_STORE(&l->owner, ATOMIC_LOAD(&l->owner, MEM_RELAXED) + 1u, MEM_RELEASE);
}

/* True iff some context holds or waits for the lock. A hint only once SMP exists. Any context. */
static inline bool rawSpinIsLocked(const RawSpinlock *l) {
    return ATOMIC_LOAD(&l->next, MEM_RELAXED) != ATOMIC_LOAD(&l->owner, MEM_RELAXED);
}

#endif
