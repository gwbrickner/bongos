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
