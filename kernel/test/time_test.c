/* ktests for the monotonic clock and the timer queue (M3.3, D-176, ROADMAP M3.3): monotonic across
 * 1M reads, one-shot timers fire once and in (deadline, arm order), cancel/re-arm/past-deadline
 * semantics, periodic re-arm from the callback, and the capacity limit. The hardware-facing
 * checks (100 ms accuracy against the PM timer, TSC and RTC) are in
 * kernel/arch/x86_64/test/time_x86_test.c.
 *
 * Every TimerObj is static, and every timer is cancelled before any KTEST_ASSERT that can return:
 * an armed timer left in a dead frame would corrupt the queue for the tests after it. Callbacks
 * run in IRQ context, so they only record into volatile globals. Waits poll with archPause()
 * against timeMonotonicNs() and never `hlt`. */
#include "irq.h"
#include "ktest.h"
#include "timekeeping.h"

#include <arch/cpu.h>
#include <arch/trap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MS 1000000ull

typedef struct {
    volatile uint32_t count;
    volatile uint64_t firedAt;
    volatile uint32_t ifSeen;
    volatile uint32_t depthSeen;
} Fire;

static void fireCb(TimerObj *t, void *ctx) {
    (void)t;
    Fire *f = (Fire *)ctx;
    f->firedAt = timeMonotonicNs();
    f->ifSeen = archInterruptsEnabled() ? 1 : 0;
    f->depthSeen = irqDepth();
    f->count++;
}

/* Polls until `*counter >= target` or `budgetNs` of monotonic time passes. */
static bool waitCount(volatile uint32_t *counter, uint32_t target, uint64_t budgetNs) {
    uint64_t start = timeMonotonicNs();
    while (*counter < target) {
        if (timeMonotonicNs() - start > budgetNs) {
            return false;
        }
        archPause();
    }
    return true;
}

static void spinNs(uint64_t ns) {
    uint64_t start = timeMonotonicNs();
    while (timeMonotonicNs() - start < ns) {
        archPause();
    }
}

KTEST(time_monotonic_1m_reads) {
    uint64_t first = timeMonotonicNs();
    uint64_t prev = first;
    uint32_t backwards = 0;
    for (uint32_t i = 0; i < 1000000; i++) {
        uint64_t now = timeMonotonicNs();
        if (now < prev) {
            backwards++;
        }
        prev = now;
    }
    KTEST_ASSERT_EQ(backwards, 0);
    KTEST_ASSERT(prev > first);
}

KTEST(time_timer_fires_once) {
    static Fire f;
    static TimerObj t;
    f.count = 0;
    timerInit(&t, fireCb, &f);
    uint64_t deadline = timeMonotonicNs() + 5 * MS;
    KTEST_ASSERT(timerArm(&t, deadline) == STATUS_OK);
    KTEST_ASSERT(timerIsArmed(&t));
    bool fired = waitCount(&f.count, 1, 500 * MS);
    spinNs(30 * MS); /* a second (spurious) firing would show up by now */
    uint32_t count = f.count;
    bool armed = timerIsArmed(&t);
    KTEST_ASSERT(fired);
    KTEST_ASSERT_EQ(count, 1);
    KTEST_ASSERT(!armed);
    KTEST_ASSERT(f.firedAt >= deadline); /* never early */
    KTEST_ASSERT_EQ(f.ifSeen, 0);
    KTEST_ASSERT_EQ(f.depthSeen, 1);
    KTEST_ASSERT(timerCancel(&t) == STATUS_ERR_NOT_FOUND); /* already ran */
}

KTEST(time_timer_cancel) {
    static Fire f;
    static TimerObj t;
    f.count = 0;
    timerInit(&t, fireCb, &f);
    KTEST_ASSERT(timerCancel(&t) == STATUS_ERR_NOT_FOUND); /* idle */
    KTEST_ASSERT(timerArm(&t, timeMonotonicNs() + 50 * MS) == STATUS_OK);
    Status first = timerCancel(&t);
    Status second = timerCancel(&t);
    spinNs(100 * MS);
    KTEST_ASSERT(first == STATUS_OK);
    KTEST_ASSERT(second == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT_EQ(f.count, 0);
}

KTEST(time_timer_rearm) {
    static Fire f;
    static TimerObj t;
    f.count = 0;
    timerInit(&t, fireCb, &f);
    uint64_t start = timeMonotonicNs();
    KTEST_ASSERT(timerArm(&t, start + 1000 * MS) == STATUS_OK);
    Status st = timerArm(&t, start + 5 * MS); /* earlier: it becomes the root again */
    bool fired = waitCount(&f.count, 1, 500 * MS);
    spinNs(30 * MS);
    uint32_t count = f.count;
    uint64_t at = f.firedAt;
    KTEST_ASSERT(st == STATUS_OK);
    KTEST_ASSERT(fired);
    KTEST_ASSERT_EQ(count, 1);
    KTEST_ASSERT(at >= start + 5 * MS && at < start + 100 * MS);

    /* The other direction: re-arm to a later deadline, which must not fire at the old one. */
    f.count = 0;
    start = timeMonotonicNs();
    KTEST_ASSERT(timerArm(&t, start + 10 * MS) == STATUS_OK);
    KTEST_ASSERT(timerArm(&t, start + 80 * MS) == STATUS_OK);
    spinNs(40 * MS);
    uint32_t early = f.count;
    bool fired2 = waitCount(&f.count, 1, 500 * MS);
    uint64_t at2 = f.firedAt;
    KTEST_ASSERT_EQ(early, 0);
    KTEST_ASSERT(fired2);
    KTEST_ASSERT(at2 >= start + 80 * MS);
}

typedef struct {
    TimerObj *t;
    uint64_t deadline;
    uint64_t firedAt[10];
    volatile uint32_t count;
    volatile uint32_t badContext;
} Periodic;

static void periodicCb(TimerObj *t, void *ctx) {
    Periodic *p = (Periodic *)ctx;
    uint32_t n = p->count;
    if (archInterruptsEnabled() || irqDepth() != 1) {
        p->badContext++;
    }
    if (n < 10) {
        p->firedAt[n] = timeMonotonicNs();
    }
    p->count = n + 1;
    if (n + 1 < 10) {
        p->deadline += 10 * MS;
        (void)timerArm(t, p->deadline);
    }
}

KTEST(time_timer_periodic_from_callback) {
    static Periodic p;
    static TimerObj t;
    p.count = 0;
    p.badContext = 0;
    p.t = &t;
    timerInit(&t, periodicCb, &p);
    uint64_t start = timeMonotonicNs();
    p.deadline = start + 10 * MS;
    uint64_t first = p.deadline;
    KTEST_ASSERT(timerArm(&t, p.deadline) == STATUS_OK);
    bool done = waitCount(&p.count, 10, 1000 * MS);
    if (!done) {
        (void)timerCancel(&t);
    }
    spinNs(30 * MS);
    uint32_t count = p.count;
    KTEST_ASSERT(done);
    KTEST_ASSERT_EQ(count, 10);
    KTEST_ASSERT_EQ(p.badContext, 0);
    for (uint32_t i = 0; i < 10; i++) {
        KTEST_ASSERT(p.firedAt[i] >= first + i * 10 * MS); /* never ahead of its own deadline */
    }
    KTEST_ASSERT(p.firedAt[9] < first + 9 * 10 * MS + 100 * MS);
}

KTEST(time_timer_past_deadline_fires) {
    static Fire f;
    static TimerObj t;
    f.count = 0;
    timerInit(&t, fireCb, &f);
    uint64_t now = timeMonotonicNs();
    uint64_t past = now > MS ? now - MS : 0;
    uint64_t flags = archIrqSave();
    Status st = timerArm(&t, past);
    uint32_t insideSection = f.count; /* never synchronous */
    archIrqRestore(flags);
    bool fired = waitCount(&f.count, 1, 50 * MS);
    if (!fired) {
        (void)timerCancel(&t);
    }
    KTEST_ASSERT(st == STATUS_OK);
    KTEST_ASSERT_EQ(insideSection, 0);
    KTEST_ASSERT(fired);
}

typedef struct {
    volatile uint32_t count;
    volatile uint32_t order[8];
} Order;

typedef struct {
    Order *o;
    uint32_t index;
} OrderCtx;

static void orderCb(TimerObj *t, void *ctx) {
    (void)t;
    OrderCtx *c = (OrderCtx *)ctx;
    uint32_t n = c->o->count;
    if (n < 8) {
        c->o->order[n] = c->index;
    }
    c->o->count = n + 1;
}

KTEST(time_timer_order) {
    static Order o;
    static OrderCtx ctx[8];
    static TimerObj t[8];
    static const uint64_t ms[8] = {40, 20, 60, 20, 80, 10, 70, 30}; /* indexes 1 and 3 tie */
    static const uint32_t expect[8] = {5, 1, 3, 7, 0, 2, 6, 4};     /* (deadline, arm order) */
    o.count = 0;
    uint64_t base = timeMonotonicNs() + 20 * MS;
    Status st = STATUS_OK;
    for (uint32_t i = 0; i < 8; i++) {
        ctx[i].o = &o;
        ctx[i].index = i;
        timerInit(&t[i], orderCb, &ctx[i]);
        Status s = timerArm(&t[i], base + ms[i] * MS);
        if (s != STATUS_OK) {
            st = s;
        }
    }
    bool done = waitCount(&o.count, 8, 1000 * MS);
    for (uint32_t i = 0; i < 8; i++) {
        (void)timerCancel(&t[i]);
    }
    KTEST_ASSERT(st == STATUS_OK);
    KTEST_ASSERT(done);
    KTEST_ASSERT_EQ(o.count, 8);
    for (uint32_t i = 0; i < 8; i++) {
        KTEST_ASSERT_EQ(o.order[i], expect[i]);
    }
}

KTEST(time_timer_misuse) {
    static Fire f;
    static TimerObj extra;
    static TimerObj many[TIMER_HEAP_CAP];
    KTEST_ASSERT(timerArm(NULL, 0) == STATUS_ERR_INVALID);
    KTEST_ASSERT(timerCancel(NULL) == STATUS_ERR_INVALID);
    TimerObj noFn = {0}; /* never passed through timerInit(): no callback */
    KTEST_ASSERT(timerArm(&noFn, timeMonotonicNs() + 1000 * MS) == STATUS_ERR_INVALID);

    uint64_t far = timeMonotonicNs() + 100000 * MS;
    uint32_t armed = 0;
    for (uint32_t i = 0; i < TIMER_HEAP_CAP; i++) {
        timerInit(&many[i], fireCb, &f);
        if (timerArm(&many[i], far + i) == STATUS_OK) {
            armed++;
        }
    }
    timerInit(&extra, fireCb, &f);
    Status full = timerArm(&extra, far);
    bool extraArmed = timerIsArmed(&extra);
    /* A full queue still lets an armed timer be re-armed (it frees its own slot first). */
    Status rearm = timerArm(&many[7], far + 5000);
    for (uint32_t i = 0; i < TIMER_HEAP_CAP; i++) {
        (void)timerCancel(&many[i]);
    }
    KTEST_ASSERT_EQ(armed, TIMER_HEAP_CAP);
    KTEST_ASSERT(full == STATUS_ERR_NO_MEMORY);
    KTEST_ASSERT(!extraArmed);
    KTEST_ASSERT(rearm == STATUS_OK);
    KTEST_ASSERT(timerArm(&extra, far) == STATUS_OK); /* room again */
    KTEST_ASSERT(timerCancel(&extra) == STATUS_OK);
}

/* --- bug-sweeper (M3.3 finish): adversarial queue tests ------------------------------------- */

/* One batch: A, B and C are all due when the interrupt runs (armed with IRQs off, deadline = now),
 * so they are collected together in arm order. A's callback re-arms B (popped, not started) into
 * the future and cancels C (popped, not started): B must not run in this batch and must fire once
 * at its new deadline; C must never run. A's callback also arms a fresh timer D in the past, which
 * must run from a later interrupt, never from this batch. */
typedef struct {
    TimerObj *b, *c, *d;
    volatile uint64_t bDeadline;
    volatile int32_t cancelC;
    volatile int32_t armB, armD;
    volatile uint64_t batchVecCount;
} BatchA;

static BatchA batchA;
static Fire batchFireB, batchFireC, batchFireD;
static volatile uint64_t batchVecCountD;

static void batchCbA(TimerObj *t, void *ctx) {
    (void)t;
    BatchA *a = (BatchA *)ctx;
    a->batchVecCount = irqVectorCount(0xFE);
    a->bDeadline = timeMonotonicNs() + 20 * MS;
    a->armB = (int32_t)timerArm(a->b, a->bDeadline);
    a->cancelC = (int32_t)timerCancel(a->c);
    a->armD = (int32_t)timerArm(a->d, 0);
}

static void batchCbD(TimerObj *t, void *ctx) {
    batchVecCountD = irqVectorCount(0xFE);
    fireCb(t, ctx);
}

KTEST(time_timer_batch_sibling_rearm_cancel) {
    static TimerObj a, b, c, d;
    batchFireB.count = batchFireC.count = batchFireD.count = 0;
    batchA.b = &b;
    batchA.c = &c;
    batchA.d = &d;
    batchA.cancelC = 99;
    batchA.armB = 99;
    batchA.armD = 99;
    batchVecCountD = 0;
    timerInit(&a, batchCbA, &batchA);
    timerInit(&b, fireCb, &batchFireB);
    timerInit(&c, fireCb, &batchFireC);
    timerInit(&d, batchCbD, &batchFireD);
    uint64_t f = archIrqSave();
    uint64_t now = timeMonotonicNs();
    Status sa = timerArm(&a, now);
    Status sb = timerArm(&b, now);
    Status sc = timerArm(&c, now);
    archIrqRestore(f);
    bool dFired = waitCount(&batchFireD.count, 1, 200 * MS);
    bool bFired = waitCount(&batchFireB.count, 1, 500 * MS);
    spinNs(30 * MS);
    uint32_t bCount = batchFireB.count, cCount = batchFireC.count, dCount = batchFireD.count;
    (void)timerCancel(&a);
    (void)timerCancel(&b);
    (void)timerCancel(&c);
    (void)timerCancel(&d);
    KTEST_ASSERT(sa == STATUS_OK && sb == STATUS_OK && sc == STATUS_OK);
    KTEST_ASSERT_EQ(batchA.armB, STATUS_OK);
    KTEST_ASSERT_EQ(batchA.cancelC, STATUS_OK); /* popped but not started: cancellable */
    KTEST_ASSERT_EQ(batchA.armD, STATUS_OK);
    KTEST_ASSERT(dFired);
    KTEST_ASSERT(bFired);
    KTEST_ASSERT_EQ(bCount, 1);
    KTEST_ASSERT_EQ(cCount, 0);
    KTEST_ASSERT_EQ(dCount, 1);
    KTEST_ASSERT(batchFireB.firedAt >= batchA.bDeadline); /* not run in A's batch */
    KTEST_ASSERT(batchVecCountD > batchA.batchVecCount);  /* D ran from a later interrupt */
}

/* A callback that re-arms itself in the past runs once per interrupt, never in a loop inside one
 * handler (D-176): every run sees a larger 0xFE interrupt count than the run before it. */
typedef struct {
    volatile uint32_t count;
    volatile uint32_t sameIrq;
    volatile uint64_t lastVec;
    volatile int32_t armFail;
} PastLoop;

static void pastLoopCb(TimerObj *t, void *ctx) {
    PastLoop *p = (PastLoop *)ctx;
    uint64_t v = irqVectorCount(0xFE);
    if (p->count != 0 && v <= p->lastVec) {
        p->sameIrq++;
    }
    p->lastVec = v;
    p->count++;
    if (p->count < 50) {
        Status st = timerArm(t, 0);
        if (st != STATUS_OK) {
            p->armFail++;
        }
    }
}

KTEST(time_timer_past_rearm_once_per_irq) {
    static PastLoop p;
    static TimerObj t;
    p.count = 0;
    p.sameIrq = 0;
    p.lastVec = 0;
    p.armFail = 0;
    timerInit(&t, pastLoopCb, &p);
    KTEST_ASSERT(timerArm(&t, 0) == STATUS_OK);
    bool done = waitCount(&p.count, 50, 1000 * MS);
    if (!done) {
        (void)timerCancel(&t);
    }
    spinNs(10 * MS);
    uint32_t count = p.count;
    KTEST_ASSERT(done);
    KTEST_ASSERT_EQ(count, 50);
    KTEST_ASSERT_EQ(p.sameIrq, 0);
    KTEST_ASSERT_EQ(p.armFail, 0);
    KTEST_ASSERT(!timerIsArmed(&t));
}

/* Deadlines near and at UINT64_MAX (the `deadline - now` and 2^40 ns clamp paths) never fire and
 * never disturb a near timer; they stay armed and cancel cleanly. */
KTEST(time_timer_far_deadlines) {
    static Fire far, near;
    static TimerObj f1, f2, f3, n;
    far.count = 0;
    near.count = 0;
    timerInit(&f1, fireCb, &far);
    timerInit(&f2, fireCb, &far);
    timerInit(&f3, fireCb, &far);
    timerInit(&n, fireCb, &near);
    uint64_t now = timeMonotonicNs();
    Status s1 = timerArm(&f1, UINT64_MAX);
    Status s2 = timerArm(&f2, UINT64_MAX - 1);
    Status s3 = timerArm(&f3, now + (1ull << 41));
    uint64_t nd = timeMonotonicNs() + 10 * MS;
    Status s4 = timerArm(&n, nd);
    bool nearFired = waitCount(&near.count, 1, 500 * MS);
    spinNs(30 * MS);
    bool armed = timerIsArmed(&f1) && timerIsArmed(&f2) && timerIsArmed(&f3);
    Status c1 = timerCancel(&f1), c2 = timerCancel(&f2), c3 = timerCancel(&f3);
    (void)timerCancel(&n);
    KTEST_ASSERT(s1 == STATUS_OK && s2 == STATUS_OK && s3 == STATUS_OK && s4 == STATUS_OK);
    KTEST_ASSERT(nearFired);
    KTEST_ASSERT(near.firedAt >= nd);
    KTEST_ASSERT_EQ(far.count, 0);
    KTEST_ASSERT(armed);
    KTEST_ASSERT(c1 == STATUS_OK && c2 == STATUS_OK && c3 == STATUS_OK);
}

/* 256 timers due together run from one batch in arm order (the FIFO tie-break), each once. */
typedef struct {
    volatile uint32_t count;
    volatile uint32_t outOfOrder;
    volatile uint64_t firstVec, lastVec;
} Burst;

static Burst burst;
static uint32_t burstIndex[TIMER_HEAP_CAP];

static void burstCb(TimerObj *t, void *ctx) {
    (void)t;
    uint32_t idx = *(uint32_t *)ctx;
    if (idx != burst.count) {
        burst.outOfOrder++;
    }
    if (burst.count == 0) {
        burst.firstVec = irqVectorCount(0xFE);
    }
    burst.lastVec = irqVectorCount(0xFE);
    burst.count++;
}

KTEST(time_timer_full_queue_one_batch) {
    static TimerObj many[TIMER_HEAP_CAP];
    burst.count = 0;
    burst.outOfOrder = 0;
    Status st = STATUS_OK;
    uint64_t f = archIrqSave();
    uint64_t dl = timeMonotonicNs();
    for (uint32_t i = 0; i < TIMER_HEAP_CAP; i++) {
        burstIndex[i] = i;
        timerInit(&many[i], burstCb, &burstIndex[i]);
        Status s = timerArm(&many[i], dl);
        if (s != STATUS_OK) {
            st = s;
        }
    }
    archIrqRestore(f);
    bool done = waitCount(&burst.count, TIMER_HEAP_CAP, 500 * MS);
    spinNs(10 * MS);
    uint32_t count = burst.count;
    for (uint32_t i = 0; i < TIMER_HEAP_CAP; i++) {
        (void)timerCancel(&many[i]);
    }
    KTEST_ASSERT(st == STATUS_OK);
    KTEST_ASSERT(done);
    KTEST_ASSERT_EQ(count, TIMER_HEAP_CAP);
    KTEST_ASSERT_EQ(burst.outOfOrder, 0);
    KTEST_ASSERT_EQ(burst.firstVec, burst.lastVec);
}

/* Cancelling and re-arming itself from its own callback: cancel says "already started"
 * (NOT_FOUND); a re-arm followed by a cancel leaves nothing to fire. */
typedef struct {
    volatile uint32_t count;
    volatile int32_t cancelSelf, rearm, cancelAfterRearm;
} SelfCtl;

static void selfCtlCb(TimerObj *t, void *ctx) {
    SelfCtl *s = (SelfCtl *)ctx;
    s->count++;
    s->cancelSelf = (int32_t)timerCancel(t);
    s->rearm = (int32_t)timerArm(t, 0);
    s->cancelAfterRearm = (int32_t)timerCancel(t);
}

KTEST(time_timer_callback_cancels_self) {
    static SelfCtl s;
    static TimerObj t;
    s.count = 0;
    s.cancelSelf = s.rearm = s.cancelAfterRearm = 99;
    timerInit(&t, selfCtlCb, &s);
    KTEST_ASSERT(timerArm(&t, timeMonotonicNs() + 2 * MS) == STATUS_OK);
    bool fired = waitCount(&s.count, 1, 500 * MS);
    spinNs(30 * MS);
    uint32_t count = s.count;
    (void)timerCancel(&t);
    KTEST_ASSERT(fired);
    KTEST_ASSERT_EQ(count, 1);
    KTEST_ASSERT_EQ(s.cancelSelf, STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT_EQ(s.rearm, STATUS_OK);
    KTEST_ASSERT_EQ(s.cancelAfterRearm, STATUS_OK);
}

/* timerInit's panicBug() misuse checks actually fire (D-082). */
static void timerInitNullTimer(void *arg) {
    (void)arg;
    timerInit(NULL, fireCb, NULL);
}

static void timerInitNullFn(void *arg) {
    timerInit((TimerObj *)arg, NULL, NULL);
}

KTEST(time_timer_init_misuse_panics) {
    static TimerObj t;
    TrapCatchInfo info;
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, timerInitNullTimer, NULL, &info));
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_KERNEL_BUG);
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, timerInitNullFn, &t, &info));
    KTEST_ASSERT_EQ(info.kind, TRAP_CATCH_KERNEL_BUG);
}
