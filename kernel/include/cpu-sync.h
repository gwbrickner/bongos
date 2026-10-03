/* The synchronization state of one CPU (D-184). A member of CpuLocal (cpu-local.h, D-190); split
 * out so preempt.h and the lock validator can name the type without the whole CpuLocal. */
#ifndef KERNEL_CPU_SYNC_H
#define KERNEL_CPU_SYNC_H

#include "lockdep.h"

#include <stdint.h>

typedef struct CpuSync {
    uint32_t preemptCount;
    uint32_t klogHeld; /* klog's sink section is open on this CPU (klog.c: lets an exception that
                          logs and resumes re-enter) */
#ifdef KERNEL_DEBUG
    uint32_t lockdepRecursion; /* validator re-entry guard; IRQs are off while it is nonzero */
    LockdepHeldStack held;     /* this CPU's held locks, for the validator */
#endif
} CpuSync;

#endif
