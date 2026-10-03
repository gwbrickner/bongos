/* ktests for the lock validator (M3.4, D-186/D-187), debug builds only. A deliberately wrong
 * sequence runs inside lockdepExpectBegin()/End(): the validator prints its full report marked
 * "(expected by ktest)" and carries on, and the test asserts exactly one report was seen.
 * Every test uses its own locks (class usage bits and edges persist for the whole run). The
 * helpers that take locks are noinline so their names appear in the report's stacks, which
 * mk/test.mk checks. */
#include "ktest.h"
#include "lockdep.h"
#include "preempt.h"
#include "smp.h"
#include "spinlock.h"
#include "timekeeping.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef KERNEL_DEBUG

#include "kmalloc.h"
#include "klog.h"
#include "pmm.h"
#include "random.h"
#include "vmalloc.h"
#include "vmm.h"

static Spinlock invA = SPINLOCK_INIT("lt-inv-a");
static Spinlock invB = SPINLOCK_INIT("lt-inv-b");

static __attribute__((noinline)) void lockdepTestTakeAB(void) {
    uint64_t fa = spinLockIrqSave(&invA);
    uint64_t fb = spinLockIrqSave(&invB);
    spinUnlockIrqRestore(&invB, fb);
    spinUnlockIrqRestore(&invA, fa);
}

static __attribute__((noinline)) void lockdepTestTakeBA(void) {
    uint64_t fb = spinLockIrqSave(&invB);
    uint64_t fa = spinLockIrqSave(&invA);
    spinUnlockIrqRestore(&invA, fa);
    spinUnlockIrqRestore(&invB, fb);
}

/* ROADMAP M3.4 test 1: a deliberate A->B / B->A inversion is reported. */
KTEST(lockdep_inversion_reported) {
    lockdepTestTakeAB();
    bool ab = lockdepDependsOn("lt-inv-a", "lt-inv-b");
    lockdepExpectBegin(LOCKDEP_REPORT_INVERSION);
    lockdepTestTakeBA();
    uint32_t reports = lockdepExpectEnd();
    bool ba = lockdepDependsOn("lt-inv-b", "lt-inv-a");
    KTEST_ASSERT(ab);
    KTEST_ASSERT_EQ(reports, 1);
    KTEST_ASSERT(!ba); /* the offending edge is not recorded */
}

static Spinlock irqUnsafeLock = SPINLOCK_INIT("lt-irq-unsafe");
static volatile uint32_t irqCallbackRan;

static __attribute__((noinline)) void lockdepTestTakeIrqsOn(Spinlock *l) {
    spinLock(l); /* IF=1: this lock is IRQ-unsafe from now on */
    spinUnlock(l);
}

static __attribute__((noinline)) void lockdepTestIrqCallback(TimerObj *t, void *ctx) {
    (void)t;
    Spinlock *l = (Spinlock *)ctx;
    spinLock(l); /* hard-IRQ context */
    spinUnlock(l);
    irqCallbackRan = 1;
}

/* Fires `lockdepTestIrqCallback` on `l` from the timer interrupt and waits for it. */
static bool runIrqCallback(Spinlock *l) {
    static TimerObj timer;
    irqCallbackRan = 0;
    timerInit(&timer, lockdepTestIrqCallback, l);
    if (timerArm(&timer, timeMonotonicNs()) != STATUS_OK) {
        return false;
    }
    uint64_t deadline = timeMonotonicNs() + 1000000000ull;
    while (!irqCallbackRan && timeMonotonicNs() < deadline) {
        archPause();
    }
    if (!irqCallbackRan) {
        (void)timerCancel(&timer);
        return false;
    }
    return true;
}

/* ROADMAP M3.4 test 2: an IRQ-unsafe lock taken from an IRQ is reported. */
KTEST(lockdep_irq_unsafe_in_irq_reported) {
    lockdepTestTakeIrqsOn(&irqUnsafeLock);
    lockdepExpectBegin(LOCKDEP_REPORT_IRQ_INCONSISTENT);
    bool ran = runIrqCallback(&irqUnsafeLock);
    uint32_t reports = lockdepExpectEnd();
    KTEST_ASSERT(ran);
    KTEST_ASSERT_EQ(reports, 1);
}

static Spinlock irqSafeLock = SPINLOCK_INIT("lt-irq-safe");

/* The other direction: used in a handler first, then taken with IRQs enabled. */
KTEST(lockdep_irq_safe_then_irqs_on_reported) {
    bool ran = runIrqCallback(&irqSafeLock);
    KTEST_ASSERT(ran);
    lockdepExpectBegin(LOCKDEP_REPORT_IRQ_INCONSISTENT);
    lockdepTestTakeIrqsOn(&irqSafeLock);
    uint32_t reports = lockdepExpectEnd();
    KTEST_ASSERT_EQ(reports, 1);
}

static Spinlock suSafe = SPINLOCK_INIT("lt-su-safe");
static Spinlock suUnsafe = SPINLOCK_INIT("lt-su-unsafe");

/* D-201: a lock taken in a hard IRQ must not be held while taking one that is also taken with IRQs
 * enabled: if the interrupt lands while process context holds the unsafe one and the handler holds
 * the safe one and waits for it, nothing makes progress. The unsafe lock has IRQs-on usage from an
 * earlier acquire, so the report fires even though this acquire itself runs with IRQs off. */
KTEST(lockdep_irq_safe_unsafe_reported) {
    bool ran = runIrqCallback(&suSafe);
    KTEST_ASSERT(ran);
    lockdepTestTakeIrqsOn(&suUnsafe);
    uint64_t f = spinLockIrqSave(&suSafe);
    lockdepExpectBegin(LOCKDEP_REPORT_IRQ_SAFE_UNSAFE);
    spinLock(&suUnsafe);
    uint32_t reports = lockdepExpectEnd();
    spinUnlock(&suUnsafe);
    spinUnlockIrqRestore(&suSafe, f);
    KTEST_ASSERT_EQ(reports, 1);
    KTEST_ASSERT(!lockdepDependsOn("lt-su-safe", "lt-su-unsafe")); /* no edge recorded */
}

static Spinlock cpuInvA = SPINLOCK_INIT("lt-cpu-inv-a");
static Spinlock cpuInvB = SPINLOCK_INIT("lt-cpu-inv-b");
static volatile uint32_t cpuInvReports, cpuInvRan;

static void lockdepTestCpuBody(void *arg) {
    (void)arg;
    uint64_t fa = spinLockIrqSave(&cpuInvA);
    uint64_t fb = spinLockIrqSave(&cpuInvB);
    spinUnlockIrqRestore(&cpuInvB, fb);
    spinUnlockIrqRestore(&cpuInvA, fa);
    lockdepExpectBegin(LOCKDEP_REPORT_INVERSION);
    fb = spinLockIrqSave(&cpuInvB);
    fa = spinLockIrqSave(&cpuInvA);
    spinUnlockIrqRestore(&cpuInvA, fa);
    spinUnlockIrqRestore(&cpuInvB, fb);
    cpuInvReports = lockdepExpectEnd();
    cpuInvRan = 1;
}

/* Expect mode is per CPU (D-201): a CPU other than the BSP can arm it and swallow its own report
 * (a report raised on a CPU that did not arm it still panics; that half cannot be asserted from a
 * test that must survive). */
KTEST(lockdep_expect_per_cpu) {
    cpuInvReports = 0;
    cpuInvRan = 0;
    if (smpOnlineCount() < 2) {
        lockdepTestCpuBody(NULL);
    } else {
        SmpWork w = {.fn = lockdepTestCpuBody, .arg = NULL, .remaining = 1};
        smpWorkPost(1, &w);
        smpWorkWait(&w);
    }
    KTEST_ASSERT(cpuInvRan);
    KTEST_ASSERT_EQ(cpuInvReports, 1);
    KTEST_ASSERT(!lockdepExpectArmed());
}

static Spinlock sameSite[2];

KTEST(lockdep_class_recursion_reported) {
    for (int i = 0; i < 2; i++) {
        spinInit(&sameSite[i], "lt-same-site"); /* one call site: one class */
    }
    uint64_t f0 = spinLockIrqSave(&sameSite[0]);
    lockdepExpectBegin(LOCKDEP_REPORT_RECURSION);
    uint64_t f1 = spinLockIrqSave(&sameSite[1]);
    uint32_t reports = lockdepExpectEnd();
    spinUnlockIrqRestore(&sameSite[1], f1);
    spinUnlockIrqRestore(&sameSite[0], f0);
    KTEST_ASSERT_EQ(reports, 1);
}

static Spinlock tryA = SPINLOCK_INIT("lt-try-a");
static Spinlock tryB = SPINLOCK_INIT("lt-try-b");

KTEST(lockdep_trylock_records_no_edge) {
    uint64_t f = spinLockIrqSave(&tryA);
    bool got = spinTryLock(&tryB);
    if (got) {
        spinUnlock(&tryB);
    }
    spinUnlockIrqRestore(&tryA, f);
    KTEST_ASSERT(got);
    KTEST_ASSERT(!lockdepDependsOn("lt-try-a", "lt-try-b"));
}

static Spinlock ooA = SPINLOCK_INIT("lt-oo-a");
static Spinlock ooB = SPINLOCK_INIT("lt-oo-b");
static Spinlock ooC = SPINLOCK_INIT("lt-oo-c");

KTEST(lockdep_out_of_order_release) {
    uint32_t base = lockdepHeldDepth();
    spinLock(&ooA);
    spinLock(&ooB);
    spinLock(&ooC);
    uint32_t three = lockdepHeldDepth();
    spinUnlock(&ooA); /* not the most recent */
    spinUnlock(&ooC);
    spinUnlock(&ooB);
    KTEST_ASSERT_EQ(three, base + 3);
    KTEST_ASSERT_EQ(lockdepHeldDepth(), base);
    KTEST_ASSERT(lockdepDependsOn("lt-oo-a", "lt-oo-c"));
}

static Spinlock outerLock = SPINLOCK_INIT("lt-outer");

/* Proves the existing locks are wired to the validator, so "no false positives across the existing
 * tests" (the Done-when clause) is not vacuous: each subsystem's class exists after real use. */
KTEST(lockdep_sees_kernel_locks) {
    Page *page = NULL;
    Status st = pmmAllocPages(0, 0, &page);
    KTEST_ASSERT(st == STATUS_OK);
    pmmFreePages(page, 0);
    uint64_t pa;
    VmmFlags fl;
    (void)vmmLookupKernel((uint64_t)(uintptr_t)&outerLock, &pa, &fl);
    void *p = kmalloc(64, 0);
    kfree(p);
    VmallocStats vs;
    vmallocGetStats(&vs);
    (void)randomU64();
    klogWrite(KLOG_INFO, "lockdep-test", "kernel locks exercised");

    uint64_t f = spinLockIrqSave(&outerLock);
    klogWrite(KLOG_INFO, "lockdep-test", "logging under a lock");
    spinUnlockIrqRestore(&outerLock, f);

    KTEST_ASSERT(lockdepClassRegistered("pmm"));
    KTEST_ASSERT(lockdepClassRegistered("vmm"));
    KTEST_ASSERT(lockdepClassRegistered("slab"));
    KTEST_ASSERT(lockdepClassRegistered("vmalloc"));
    KTEST_ASSERT(lockdepClassRegistered("random"));
    KTEST_ASSERT(lockdepClassRegistered("klog"));
    KTEST_ASSERT(lockdepDependsOn("lt-outer", "klog"));
}

/* ---- bug-sweeper (M3.4 finish): adversarial validator tests. Each uses its own locks. ---- */

static Spinlock nhLock = SPINLOCK_INIT("lt-not-held");

/* Releasing a lock the validator never saw taken (raw-locked behind its back) is NOT_HELD. */
KTEST(lockdep_not_held_reported) {
    uint32_t depth = lockdepHeldDepth();
    uint32_t base = preemptCount();
    preemptDisable(); /* the spinUnlock below lowers it */
    rawSpinLock(&nhLock.raw);
    lockdepExpectBegin(LOCKDEP_REPORT_NOT_HELD);
    spinUnlock(&nhLock);
    uint32_t reports = lockdepExpectEnd();
    KTEST_ASSERT_EQ(reports, 1);
    KTEST_ASSERT(!spinIsLocked(&nhLock));
    KTEST_ASSERT_EQ(lockdepHeldDepth(), depth);
    KTEST_ASSERT_EQ(preemptCount(), base);
}

static Spinlock ieLock = SPINLOCK_INIT("lt-irqs-enabled-while-held");

/* A lock used in a handler, taken irqsave, then IRQs enabled while it is still held: reported at
 * release (the third IRQ-usage direction). */
KTEST(lockdep_irqs_enabled_while_held_reported) {
    bool ran = runIrqCallback(&ieLock);
    KTEST_ASSERT(ran);
    uint64_t f = spinLockIrqSave(&ieLock);
    archEnableInterrupts(); /* the bug under test; the timer callback is not armed */
    lockdepExpectBegin(LOCKDEP_REPORT_IRQ_INCONSISTENT);
    spinUnlock(&ieLock);
    uint32_t reports = lockdepExpectEnd();
    archIrqRestore(f);
    KTEST_ASSERT_EQ(reports, 1);
}

static Spinlock trA = SPINLOCK_INIT("lt-tr-a");
static Spinlock trB = SPINLOCK_INIT("lt-tr-b");
static Spinlock trC = SPINLOCK_INIT("lt-tr-c");

static __attribute__((noinline)) void lockdepTestTakePair(Spinlock *outer, Spinlock *inner) {
    uint64_t fo = spinLockIrqSave(outer);
    uint64_t fi = spinLockIrqSave(inner);
    spinUnlockIrqRestore(inner, fi);
    spinUnlockIrqRestore(outer, fo);
}

/* A->B and B->C recorded separately; C then A closes a cycle through B. */
KTEST(lockdep_transitive_inversion_reported) {
    lockdepTestTakePair(&trA, &trB);
    lockdepTestTakePair(&trB, &trC);
    bool ac = lockdepDependsOn("lt-tr-a", "lt-tr-c");
    lockdepExpectBegin(LOCKDEP_REPORT_INVERSION);
    lockdepTestTakePair(&trC, &trA);
    uint32_t reports = lockdepExpectEnd();
    KTEST_ASSERT(ac);
    KTEST_ASSERT_EQ(reports, 1);
    KTEST_ASSERT(!lockdepDependsOn("lt-tr-c", "lt-tr-a"));
}

static Spinlock segOuter = SPINLOCK_INIT("lt-seg-outer");
static Spinlock segIrq = SPINLOCK_INIT("lt-seg-irq");

/* A handler's lock is not ordered after a lock the interrupted code holds: no edge, and the
 * reverse order in process context is then not an inversion. */
KTEST(lockdep_irq_segment_records_no_edge) {
    spinLock(&segOuter); /* IF=1: an IRQ-unsafe lock, held while the timer fires */
    uint32_t depthHeld = lockdepHeldDepth();
    bool ran = runIrqCallback(&segIrq);
    uint32_t depthAfterIrq = lockdepHeldDepth();
    spinUnlock(&segOuter);
    bool falseEdge = lockdepDependsOn("lt-seg-outer", "lt-seg-irq");
    /* Reverse order in process context (irqsave, as segIrq is IRQ-safe): not an INVERSION (an
     * inversion report would not be swallowed below and would panic). Since D-201 it is a genuine
     * IRQ-safe -> IRQ-unsafe dependency, because segOuter is also taken with IRQs enabled, and that
     * is the one report expected here. */
    uint64_t f = spinLockIrqSave(&segIrq);
    lockdepExpectBegin(LOCKDEP_REPORT_IRQ_SAFE_UNSAFE);
    uint64_t g = spinLockIrqSave(&segOuter);
    uint32_t reports = lockdepExpectEnd();
    spinUnlockIrqRestore(&segOuter, g);
    spinUnlockIrqRestore(&segIrq, f);
    KTEST_ASSERT(ran);
    KTEST_ASSERT_EQ(depthAfterIrq, depthHeld);
    KTEST_ASSERT(!falseEdge);
    KTEST_ASSERT_EQ(reports, 1);
}

static Spinlock tcSite[2];

/* A trylock of a second lock of a held class is allowed (a recursion report would panic). */
KTEST(lockdep_trylock_same_class_allowed) {
    for (int i = 0; i < 2; i++) {
        spinInit(&tcSite[i], "lt-trylock-same-class");
    }
    uint64_t f = spinLockIrqSave(&tcSite[0]);
    bool got = spinTryLock(&tcSite[1]);
    uint32_t depth = lockdepHeldDepth();
    if (got) {
        spinUnlock(&tcSite[1]);
    }
    spinUnlockIrqRestore(&tcSite[0], f);
    KTEST_ASSERT(got);
    KTEST_ASSERT(depth >= 2);
}

static Spinlock tiLock = SPINLOCK_INIT("lt-trylock-irqs-on");

/* A trylock with IRQs on also marks the class IRQ-unsafe: later use in a handler is reported. */
KTEST(lockdep_trylock_irqs_on_then_irq_reported) {
    bool got = spinTryLock(&tiLock); /* IF=1 */
    if (got) {
        spinUnlock(&tiLock);
    }
    KTEST_ASSERT(got);
    lockdepExpectBegin(LOCKDEP_REPORT_IRQ_INCONSISTENT);
    bool ran = runIrqCallback(&tiLock);
    uint32_t reports = lockdepExpectEnd();
    KTEST_ASSERT(ran);
    KTEST_ASSERT_EQ(reports, 1);
}

static Spinlock rpA = SPINLOCK_INIT("lt-repeat-a");
static Spinlock rpB = SPINLOCK_INIT("lt-repeat-b");

/* Repeating a known order adds no edge and no class. */
KTEST(lockdep_repeat_order_adds_nothing) {
    lockdepTestTakePair(&rpA, &rpB);
    LockdepStats s1;
    lockdepGetStats(&s1);
    for (int i = 0; i < 100; i++) {
        lockdepTestTakePair(&rpA, &rpB);
    }
    LockdepStats s2;
    lockdepGetStats(&s2);
    KTEST_ASSERT(s1.enabled && s2.enabled);
    KTEST_ASSERT_EQ(s2.edges, s1.edges);
    KTEST_ASSERT_EQ(s2.classes, s1.classes);
}

/* A spinInit() lock on the stack is valid (a runtime class); two different init sites are two
 * classes, so nesting them is not recursion. */
KTEST(lockdep_spininit_stack_locks) {
    Spinlock a, b;
    spinInit(&a, "lt-stack-a");
    spinInit(&b, "lt-stack-b");
    uint64_t fa = spinLockIrqSave(&a);
    uint64_t fb = spinLockIrqSave(&b);
    spinUnlockIrqRestore(&b, fb);
    spinUnlockIrqRestore(&a, fa);
    KTEST_ASSERT(lockdepClassRegistered("lt-stack-a"));
    KTEST_ASSERT(lockdepDependsOn("lt-stack-a", "lt-stack-b"));
}

/* ARCHITECTURE 7.6's documented kernel lock order holds in the recorded graph after real use:
 * the pmm never precedes vmm, slab or random, and klog (a leaf) precedes nothing. */
KTEST(lockdep_kernel_lock_order) {
    /* Exercise the vmm, vmalloc, slab and pmm paths. */
    void *v = vmalloc(3 * 4096, 0);
    KTEST_ASSERT(v != NULL);
    vfree(v);
    void *objs[64];
    for (int i = 0; i < 64; i++) {
        objs[i] = kmalloc(2048, 0);
    }
    for (int i = 0; i < 64; i++) {
        kfree(objs[i]);
    }
    static const char *const names[] = {"pmm", "vmm", "slab", "vmalloc", "random"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        KTEST_ASSERT(!lockdepDependsOn("klog", names[i]));
    }
    KTEST_ASSERT(!lockdepDependsOn("pmm", "vmm"));
    KTEST_ASSERT(!lockdepDependsOn("pmm", "slab"));
    KTEST_ASSERT(!lockdepDependsOn("pmm", "random"));
}

static Spinlock neP = SPINLOCK_INIT("lt-ne-p");
static Spinlock neQ = SPINLOCK_INIT("lt-ne-q");
static volatile uint32_t neRan;

static __attribute__((noinline)) void lockdepTestNeCallback(TimerObj *t, void *ctx) {
    (void)t;
    (void)ctx;
    spinLock(&neP);
    spinLock(&neQ); /* IRQ-unsafe lock in a handler, taken under neP */
    spinUnlock(&neQ);
    spinUnlock(&neP);
    neRan = 1;
}

/* A report that an expect window swallows must not record the dependency edge (D-187): later code
 * would otherwise inherit an order the validator had just called wrong. */
KTEST(lockdep_expected_report_records_no_edge) {
    static TimerObj timer;
    lockdepTestTakeIrqsOn(&neQ);
    neRan = 0;
    timerInit(&timer, lockdepTestNeCallback, NULL);
    lockdepExpectBegin(LOCKDEP_REPORT_IRQ_INCONSISTENT);
    bool armed = timerArm(&timer, timeMonotonicNs()) == STATUS_OK;
    uint64_t deadline = timeMonotonicNs() + 1000000000ull;
    while (armed && !neRan && timeMonotonicNs() < deadline) {
        archPause();
    }
    if (!neRan) {
        (void)timerCancel(&timer);
    }
    uint32_t reports = lockdepExpectEnd();
    KTEST_ASSERT(armed);
    KTEST_ASSERT(neRan);
    KTEST_ASSERT_EQ(reports, 1);
    KTEST_ASSERT(!lockdepDependsOn("lt-ne-p", "lt-ne-q"));
}

static Spinlock offA = SPINLOCK_INIT("lt-off-a");
static Spinlock offB = SPINLOCK_INIT("lt-off-b");

/* M3.4 sweep 2: once the validator is off (a full table), a lock taken while it was still on must
 * leave the held stack at its release; it stayed there, so irqDispatch's D-187 check panicked
 * ("returned holding a spinlock") after a table filled inside a handler. spinAssertHeld() must
 * fall back to the raw lock state while the held stacks are not maintained. */
KTEST(lockdep_disabled_release_drops_held_entry) {
    uint32_t base = lockdepHeldDepth();
    spinLock(&offA);
    uint32_t held = lockdepHeldDepth();
    lockdepTestOff();
    spinUnlock(&offA); /* taken while on, released while off */
    uint32_t afterRelease = lockdepHeldDepth();
    spinLock(&offB); /* not tracked while off */
    uint32_t whileOff = lockdepHeldDepth();
    spinAssertHeld(&offB);
    spinUnlock(&offB);
    lockdepTestOn();
    KTEST_ASSERT_EQ(held, base + 1);
    KTEST_ASSERT_EQ(afterRelease, base);
    KTEST_ASSERT_EQ(whileOff, base);
    KTEST_ASSERT_EQ(lockdepHeldDepth(), base);
}

#endif /* KERNEL_DEBUG */
