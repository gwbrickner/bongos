/* See spinlock.h. The order of operations is binding (D-183):
 *   acquire: archIrqSave (irqsave form), preemptDisable, validator, spin;
 *   release: raw release, validator, archIrqRestore, preemptEnable.
 * The validator runs before the spin so a self-deadlock is reported instead of hanging, and after
 * the raw release so a report raised while releasing klog's lock can still print through klog. */
#include "spinlock.h"

#include "lockdep.h"
#include "panic.h"
#include "preempt.h"

#include <arch/cpu.h>

#define SPIN_NOINLINE __attribute__((noinline))
#define SPIN_IP()     ((uint64_t)(uintptr_t)__builtin_return_address(0))

SPIN_NOINLINE void spinInitKeyed(Spinlock *l, const char *name, LockClassKey *key) {
    l->raw = (RawSpinlock)RAW_SPINLOCK_INIT;
#ifdef KERNEL_DEBUG
    l->dep.name = name;
    l->dep.key = key;
    l->dep.classId = 0;
#else
    (void)name;
    (void)key;
#endif
}

/* Always on: unlocking a lock nobody holds is a bug (a double unlock, or unlocking the wrong
 * lock) that would otherwise corrupt the ticket counters. */
static void spinRawRelease(Spinlock *l) {
    if (ATOMIC_LOAD(&l->raw.next, MEM_RELAXED) == ATOMIC_LOAD(&l->raw.owner, MEM_RELAXED)) {
        panicBug("spinlock: unlock of an unlocked lock (%llx)", (unsigned long long)(uintptr_t)l);
    }
    rawSpinUnlock(&l->raw);
}

SPIN_NOINLINE uint64_t spinLockIrqSave(Spinlock *l) {
    uint64_t flags = archIrqSave();
    preemptDisable();
#ifdef KERNEL_DEBUG
    lockdepAcquire(&l->dep, false, SPIN_IP());
#endif
    rawSpinLock(&l->raw);
    return flags;
}

SPIN_NOINLINE void spinLock(Spinlock *l) {
    preemptDisable();
#ifdef KERNEL_DEBUG
    lockdepAcquire(&l->dep, false, SPIN_IP());
#endif
    rawSpinLock(&l->raw);
}

SPIN_NOINLINE bool spinTryLock(Spinlock *l) {
    preemptDisable();
    if (rawSpinTryLock(&l->raw)) {
#ifdef KERNEL_DEBUG
        lockdepAcquire(&l->dep, true, SPIN_IP());
#endif
        return true;
    }
    preemptEnable();
    return false;
}

SPIN_NOINLINE void spinUnlockIrqRestore(Spinlock *l, uint64_t flags) {
    spinRawRelease(l);
#ifdef KERNEL_DEBUG
    lockdepRelease(&l->dep, SPIN_IP());
#endif
    archIrqRestore(flags);
    preemptEnable();
}

SPIN_NOINLINE void spinUnlock(Spinlock *l) {
    spinRawRelease(l);
#ifdef KERNEL_DEBUG
    lockdepRelease(&l->dep, SPIN_IP());
#endif
    preemptEnable();
}

SPIN_NOINLINE bool spinIsLocked(const Spinlock *l) {
    return rawSpinIsLocked(&l->raw);
}

SPIN_NOINLINE void spinAssertHeld(const Spinlock *l) {
#ifdef KERNEL_DEBUG
    /* The validator's held list is only maintained while it is active (a full table or a panic
     * stops it); then all that can be checked is that somebody holds the lock. */
    bool held = lockdepActive() ? lockdepIsHeld(&l->dep) : rawSpinIsLocked(&l->raw);
    if (!held) {
        panicBug("spinlock: assertion failed, lock %llx not held by this CPU",
                 (unsigned long long)(uintptr_t)l);
    }
#else
    if (!rawSpinIsLocked(&l->raw)) {
        panicBug("spinlock: assertion failed, lock %llx not held",
                 (unsigned long long)(uintptr_t)l);
    }
#endif
}
