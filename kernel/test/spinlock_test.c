/* ktests for the ticket spinlock and preemptCount (M3.4, D-183/D-184). Both profiles: the
 * validator-specific checks are in lockdep_test.c. Every test restores the state it changes before
 * any assertion that can return (the runner fails a test that leaves a lock or the count raised).
 */
#include "ktest.h"
#include "preempt.h"
#include "spinlock.h"

#include <arch/cpu.h>
#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

KTEST(spin_trylock_semantics) {
    static Spinlock lock = SPINLOCK_INIT("spin-test-trylock");
    KTEST_ASSERT(!spinIsLocked(&lock));
    uint32_t base = preemptCount();
    bool first = spinTryLock(&lock);
    uint32_t heldCount = preemptCount();
    bool locked = spinIsLocked(&lock);
    bool second = spinTryLock(&lock); /* already held: fails, count unchanged */
    uint32_t afterFail = preemptCount();
    spinUnlock(&lock);
    bool freedAgain = !spinIsLocked(&lock);
    bool third = spinTryLock(&lock);
    if (third) {
        spinUnlock(&lock);
    }
    KTEST_ASSERT(first);
    KTEST_ASSERT_EQ(heldCount, base + 1);
    KTEST_ASSERT(locked);
    KTEST_ASSERT(!second);
    KTEST_ASSERT_EQ(afterFail, base + 1);
    KTEST_ASSERT(freedAgain);
    KTEST_ASSERT(third);
    KTEST_ASSERT_EQ(preemptCount(), base);
}

KTEST(spin_irqsave_restores_if) {
    static Spinlock lock = SPINLOCK_INIT("spin-test-irqsave");
    bool ifBefore = archInterruptsEnabled();
    uint32_t base = preemptCount();
    uint64_t flags = spinLockIrqSave(&lock);
    bool ifInside = archInterruptsEnabled();
    uint32_t countInside = preemptCount();
    spinUnlockIrqRestore(&lock, flags);
    bool ifAfter = archInterruptsEnabled();

    /* Inside an outer IRQ-off section the unlock must leave IF off. */
    uint64_t outer = archIrqSave();
    uint64_t f2 = spinLockIrqSave(&lock);
    spinUnlockIrqRestore(&lock, f2);
    bool ifOuterStillOff = !archInterruptsEnabled();
    archIrqRestore(outer);

    KTEST_ASSERT(ifBefore);
    KTEST_ASSERT(!ifInside);
    KTEST_ASSERT_EQ(countInside, base + 1);
    KTEST_ASSERT(ifAfter);
    KTEST_ASSERT(ifOuterStillOff);
    KTEST_ASSERT_EQ(preemptCount(), base);
}

static void preemptUnderflowTrigger(void *arg) {
    (void)arg;
    preemptEnable(); /* count is 0 here */
}

KTEST(preempt_count_balance) {
    uint32_t base = preemptCount();
    preemptDisable();
    preemptDisable();
    uint32_t two = preemptCount();
    preemptEnable();
    uint32_t one = preemptCount();
    preemptEnable();
    KTEST_ASSERT_EQ(two, base + 2);
    KTEST_ASSERT_EQ(one, base + 1);
    KTEST_ASSERT_EQ(preemptCount(), base);
    KTEST_ASSERT(!preemptInAtomic());

    if (base == 0) {
        TrapCatchInfo info;
        bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, preemptUnderflowTrigger, NULL, &info);
        KTEST_ASSERT(caught); /* underflow is always detected */
        KTEST_ASSERT_EQ(preemptCount(), 0);
    }

    preemptDisable();
    bool atomic = preemptInAtomic();
    preemptEnable();
    KTEST_ASSERT(atomic);
}

static Spinlock freeLock = SPINLOCK_INIT("spin-test-unlock-unlocked");

static void unlockUnlockedTrigger(void *arg) {
    (void)arg;
    spinUnlock(&freeLock);
}

KTEST(spin_unlock_unlocked_caught) {
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, unlockUnlockedTrigger, NULL, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT(!spinIsLocked(&freeLock)); /* counters untouched */
    bool ok = spinTryLock(&freeLock);
    if (ok) {
        spinUnlock(&freeLock);
    }
    KTEST_ASSERT(ok);
}
