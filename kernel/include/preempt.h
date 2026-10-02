/* preemptCount plumbing (M3.4, D-184). Nothing preempts yet (M4): the count only records "this
 * CPU must not be rescheduled", and holding a Spinlock raises it. */
#ifndef KERNEL_PREEMPT_H
#define KERNEL_PREEMPT_H

#include "atomic.h"
#include "lockdep.h"

#include <stdbool.h>
#include <stdint.h>

/* The synchronization state of one CPU. Interim (D-184): there is no CpuLocal until M3.5, so this
 * is a BSP static behind cpuSync(); M3.5 points the accessor at the per-CPU area and nothing else
 * changes. */
typedef struct CpuSync {
    uint32_t preemptCount;
#ifdef KERNEL_DEBUG
    uint32_t lockdepRecursion; /* validator re-entry guard; IRQs are off while it is nonzero */
    LockdepHeldStack held;     /* this CPU's held locks, for the validator */
#endif
} CpuSync;

extern CpuSync cpuSyncBsp; /* kernel/sync/preempt.c */

static inline CpuSync *cpuSync(void) {
    return &cpuSyncBsp;
}

/* Panics (panicBug) on a preemptEnable() with a zero count. No locks. */
_Noreturn void preemptUnderflow(void);

/* Raise/lower this CPU's preemptCount. A non-atomic read-modify-write is correct here: an IRQ
 * between the load and the store always leaves the count balanced (irqDispatch checks that), and
 * the data is per-CPU. M4 must make it one `incl %gs:off` once preemption and migration exist.
 * The underflow check is always on (D-082's rationale). preemptEnable() will reschedule when the
 * count reaches zero and needResched is set (M4); nothing happens yet. IRQ-safe, never sleeps,
 * takes no lock. */
static inline void preemptDisable(void) {
    cpuSync()->preemptCount++;
    COMPILER_BARRIER();
}

static inline void preemptEnable(void) {
    COMPILER_BARRIER();
    CpuSync *s = cpuSync();
    if (s->preemptCount == 0) {
        preemptUnderflow();
    }
    s->preemptCount--;
}

static inline uint32_t preemptCount(void) {
    return cpuSync()->preemptCount;
}

/* True when the caller must not sleep: preemption is disabled, a handler is running, or
 * interrupts are off (the hook the sleeping locks and the validator's sleep check use in M4).
 * IRQ-safe. */
bool preemptInAtomic(void);

#endif
