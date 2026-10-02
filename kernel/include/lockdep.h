/* The lock validator's kernel interface (M3.4, D-186). Debug builds (KERNEL_DEBUG) track lock
 * classes, the acquisition-order graph, recursion and IRQ-safety (see lockdep-core.h for the
 * rules); release builds compile every hook to nothing. Only spinlock.c calls
 * lockdepAcquire/lockdepRelease; the Expect* and query functions are for ktests. */
#ifndef KERNEL_LOCKDEP_H
#define KERNEL_LOCKDEP_H

#include "lockdep-core.h"

#include <stdbool.h>
#include <stdint.h>

/* One per spinInit() call site (a `static LockClassKey`), so all locks initialized there share a
 * class. It must have a size so every key has its own address. */
typedef struct LockClassKey {
    uint8_t unused;
} LockClassKey;

#ifdef KERNEL_DEBUG

/* The validator's per-lock state, embedded in Spinlock in debug builds. */
typedef struct LockdepMap {
    const char *name;
    const LockClassKey *key; /* NULL for SPINLOCK_INIT locks: their own address is the class key */
    uint16_t classId;        /* cached; 0 until first use */
} LockdepMap;

/* Called by spinlock.c before the lock is spun on (a self-deadlock is reported instead of hanging)
 * and after it is raw-released. `ip` is the caller's return address. IRQ-safe, never sleeps, takes
 * no Spinlock; a no-op once a panic is in progress or after the validator disabled itself. A real
 * finding prints a report and panics (never returns) unless a ktest armed lockdepExpectBegin() for
 * that kind. */
void lockdepAcquire(LockdepMap *m, bool trylock, uint64_t ip);
void lockdepRelease(LockdepMap *m, uint64_t ip);

/* True iff this CPU's held-lock stack has `m`. IRQ-safe. */
bool lockdepIsHeld(const LockdepMap *m);

/* Locks the validator sees held on this CPU right now. IRQ-safe. */
uint32_t lockdepHeldDepth(void);

/* ktest only (D-187): from now on a report of `kind` is printed as "LOCKDEP (expected by ktest)"
 * and execution continues instead of panicking; the offending dependency edge is not recorded.
 * Panics unless a ktest is running, no handler is running, nothing is armed yet, and `kind` is one
 * of INVERSION, RECURSION, IRQ_INCONSISTENT, NOT_HELD. lockdepExpectEnd() disarms and returns how
 * many reports of that kind were swallowed. Another kind still panics. */
void lockdepExpectBegin(LockdepVerdict kind);
uint32_t lockdepExpectEnd(void);
bool lockdepExpectArmed(void);

/* ktest queries (by class name, first match). */
bool lockdepClassRegistered(const char *name);
bool lockdepDependsOn(const char *fromName, const char *toName);

typedef struct LockdepStats {
    bool enabled; /* false once a table filled up */
    uint32_t classes, edges, expectedReports;
} LockdepStats;
void lockdepGetStats(LockdepStats *out);

#else

static inline uint32_t lockdepHeldDepth(void) {
    return 0;
}
static inline bool lockdepExpectArmed(void) {
    return false;
}

#endif

#endif
