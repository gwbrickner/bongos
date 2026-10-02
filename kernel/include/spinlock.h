/* Ticket spinlocks (M3.4, D-183). A Spinlock is FIFO-fair, raises preemptCount while held, and in
 * debug builds registers with the lock validator (lockdep.h). Every function is out of line and
 * noinline so the validator records the true call site.
 *
 * Which variant: a lock that any interrupt handler ever takes must be taken with
 * spinLockIrqSave() everywhere (else a handler on the same CPU spins on a lock its own interrupted
 * code holds); the validator reports a lock taken in a handler and also with IRQs enabled. A lock
 * never taken from a handler may use spinLock(). Locks never sleep, and nothing may be held across
 * anything that can sleep (M4). */
#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include "lockdep.h"
#include "spinlock-raw.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Spinlock {
    RawSpinlock raw;
#ifdef KERNEL_DEBUG
    LockdepMap dep;
#endif
} Spinlock; /* 8 bytes in release, 32 in debug (D-183): .kmods must be built with the kernel's
               profile */

/* Static initialization, for locks with static storage duration ONLY (file scope or function
 * `static`): the lock's own address becomes its validator class (the debug build checks it lies in
 * .data/.bss and reports BAD_INIT otherwise). `nameStr` must be a string literal. */
#ifdef KERNEL_DEBUG
#define SPINLOCK_INIT(nameStr)                                                                     \
    {                                                                                              \
        RAW_SPINLOCK_INIT, {                                                                       \
            (nameStr), NULL, 0                                                                     \
        }                                                                                          \
    }
/* Runtime initialization of a lock in any storage: one validator class per call site. */
#define spinInit(l, nameStr)                                                                       \
    do {                                                                                           \
        static LockClassKey spinKey_;                                                              \
        spinInitKeyed((l), (nameStr), &spinKey_);                                                  \
    } while (0)
#else
#define SPINLOCK_INIT(nameStr)                                                                     \
    { RAW_SPINLOCK_INIT }
#define spinInit(l, nameStr) spinInitKeyed((l), (nameStr), NULL)
#endif

/* Use spinInit(), not this directly. Unlocks `l`. No locks held; the lock must not be in use. */
void spinInitKeyed(Spinlock *l, const char *name, LockClassKey *key);

/* Acquire: preemptDisable(), validator check, then spin. May spin forever if the holder never
 * releases (SMP, M3.5, adds a watchdog). Not IRQ-safe against handlers that take `l` (use the
 * IrqSave form). Any context. */
void spinLock(Spinlock *l);

/* Takes `l` only if it is free; never spins. Returns true if taken. A trylock adds no order
 * dependency in the validator. Any context. */
bool spinTryLock(Spinlock *l);

/* Release. The caller must hold `l`: unlocking a free lock panics (panicBug, always on). */
void spinUnlock(Spinlock *l);

/* Saves RFLAGS and disables IRQs, then acquires; returns the saved RFLAGS for
 * spinUnlockIrqRestore() (the same value archIrqSave() returns). */
uint64_t spinLockIrqSave(Spinlock *l);

/* Releases `l`, then restores the interrupt state `flags` captured. */
void spinUnlockIrqRestore(Spinlock *l, uint64_t flags);

/* True iff `l` is held or waited for; a hint only once SMP exists. */
bool spinIsLocked(const Spinlock *l);

/* Panics (panicBug) unless `l` is held (debug: by this CPU, per the validator; release:
 * spinIsLocked). For "caller must hold X" contracts. */
void spinAssertHeld(const Spinlock *l);

#endif
