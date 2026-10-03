/* preemptCount plumbing (M3.4, D-184). Nothing preempts yet (M4): the count only records "this
 * CPU must not be rescheduled", and holding a Spinlock raises it. */
#ifndef KERNEL_PREEMPT_H
#define KERNEL_PREEMPT_H

#include "atomic.h"
#include "cpu-local.h"
#include "cpu-sync.h"

#include <stdbool.h>
#include <stdint.h>

/* cpuSync() is this CPU's CpuSync inside its CpuLocal (D-184, D-190). */
static inline CpuSync *cpuSync(void) {
    return &cpuLocal()->sync;
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
