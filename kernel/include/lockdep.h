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
 * no Spinlock; a no-op once a panic is in progress, and after the validator disabled itself (except
 * that a release still drops the lock from this CPU's held stack, see lockdepHeldDepth()). A real
 * finding prints a report and panics (never returns) unless a ktest armed lockdepExpectBegin() for
 * that kind. */
void lockdepAcquire(LockdepMap *m, bool trylock, uint64_t ip);
void lockdepRelease(LockdepMap *m, uint64_t ip);

/* True while the validator is tracking locks (not disabled by a full table, no panic in progress);
 * its held-lock stacks are only meaningful then. IRQ-safe. */
bool lockdepActive(void);

/* True iff this CPU's held-lock stack has `m`. IRQ-safe. */
bool lockdepIsHeld(const LockdepMap *m);

/* Locks the validator sees held on this CPU right now. IRQ-safe. */
uint32_t lockdepHeldDepth(void);

/* ktest only (D-187): from now on a report of `kind` is printed as "LOCKDEP (expected by ktest)"
 * and execution continues instead of panicking; the offending dependency edge is not recorded.
 * Panics unless a ktest is running (ktestCurrentName() != NULL), no handler is running, nothing is
 * armed yet, and `kind` is one of INVERSION, RECURSION, IRQ_INCONSISTENT, NOT_HELD,
 * IRQ_SAFE_UNSAFE. Only a report raised on the arming CPU is swallowed (D-201). lockdepExpectEnd()
 * disarms and returns how many reports of that kind were swallowed. Another kind still panics, and
 * so does a RECURSION on the very same lock (it would deadlock). A swallowed report records no
 * dependency edges. */
void lockdepExpectBegin(LockdepVerdict kind);
uint32_t lockdepExpectEnd(void);
bool lockdepExpectArmed(void);

/* ktest only (D-189): turn the validator off exactly as a full table does, and back on, so a test
 * can check what code does while it is off. Every lock taken between Off and On must also be
 * released there (it is not on the held stack, so releasing it after On would be NOT_HELD). Off
 * panics unless a ktest is running, no handler is running and the validator is on; On panics unless
 * Off turned it off (a real full table stays off). A test that returns with it still off fails
 * (the runner turns it back on, lockdepTestOffReset()). */
void lockdepTestOff(void);
void lockdepTestOn(void);

/* ktest runner only, between tests: if the test that just ran left the validator off with
 * lockdepTestOff(), turns it back on and returns true (the runner fails that test); else false.
 * No locks; process context. */
bool lockdepTestOffReset(void);

/* ktest queries (by class name, first match). Both take the validator's raw graph lock with IRQs
 * off for the duration, so they are IRQ-safe and never sleep, but must not be called from inside
 * the validator or klog's sink section. lockdepClassRegistered: true iff a class of that name has
 * been registered. lockdepDependsOn: true iff the graph has a chain of order edges from the first
 * class to the second (false if either is unknown). */
bool lockdepClassRegistered(const char *name);
bool lockdepDependsOn(const char *fromName, const char *toName);

typedef struct LockdepStats {
    bool enabled; /* false once a table filled up */
    uint32_t classes, edges, expectedReports;
} LockdepStats;
/* Snapshot of the validator's counters, for the ktest runner's summary line. Same locking and
 * contexts as the queries above. */
void lockdepGetStats(LockdepStats *out);

#else

static inline uint32_t lockdepHeldDepth(void) {
    return 0;
}
static inline bool lockdepExpectArmed(void) {
    return false;
}
static inline bool lockdepTestOffReset(void) {
    return false;
}

#endif

#endif
