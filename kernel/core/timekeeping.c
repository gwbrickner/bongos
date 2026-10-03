/* Monotonic and wall clocks and the per-CPU timer queue (M3.3, D-176). See timekeeping.h. */
#include "timekeeping.h"

#include "cpu-local.h"
#include "klog.h"
#include "panic.h"

#include <arch/cpu.h>
#include <arch/timer.h>
#include <stddef.h>

#define TIMER_MAX_DELTA_NS (1ull << 40)

/* One CPU's queue; reached through cpuLocal()->timer (D-190, D-199). */
typedef struct {
    TimerHeap heap;
    bool expiring; /* the interrupt handler is running callbacks; it reprograms when done */
} TimerCpu;

static TimerCpu bspTimer;

static inline TimerCpu *timerCpu(void) {
    return (TimerCpu *)cpuLocal()->timer;
}

static bool inited;      /* the clock works (set before the LAPIC timer is calibrated) */
static bool timersReady; /* the LAPIC timer is calibrated and programmed: timerArm() may be used */
static uint64_t tscHzValue;
static uint64_t tscBase;
static uint64_t tscToNs; /* TSC ticks -> ns, 32.32 fixed point */
static uint64_t wallBaseNs;
static bool wallOk;

uint64_t timeMonotonicNs(void) {
    if (!inited) {
        return 0;
    }
    return timeScale(archReadTscOrdered() - tscBase, tscToNs);
}

uint64_t timeWallNs(void) {
    return wallBaseNs + timeMonotonicNs();
}

bool timeWallValid(void) {
    return wallOk;
}

uint64_t timeTscHz(void) {
    return tscHzValue;
}

/* Caller holds the IRQ-disable section. */
static void reprogram(void) {
    TimerCpu *tc = timerCpu();
    if (tc->expiring) {
        return;
    }
    TimerObj *root = timerHeapPeek(&tc->heap);
    if (root == NULL) {
        archTimerStop();
        return;
    }
    uint64_t now = timeMonotonicNs();
    uint64_t delta = root->deadlineNs <= now ? 0 : root->deadlineNs - now;
    archTimerSet(delta > TIMER_MAX_DELTA_NS ? TIMER_MAX_DELTA_NS : delta);
}

void timeReprogram(void) {
    reprogram();
}

void timeInit(void) {
    if (inited) {
        panic("time: timeInit() called twice");
    }
    uint64_t hz;
    archClockInit(&hz);
    uint64_t mult;
    if (timeMakeMult(hz, 1000000000ull, &mult) != STATUS_OK) {
        panic("time: cannot scale a %llu Hz TSC to nanoseconds", (unsigned long long)hz);
    }
    timerHeapInit(&bspTimer.heap);
    bspTimer.expiring = false;
    cpuLocal()->timer = &bspTimer;
    tscHzValue = hz;
    tscToNs = mult;
    tscBase = archReadTscOrdered();
    inited = true;

    archTimerInit(hz);
    timersReady = true;

    TimeRtcRaw raw;
    archRtcRead(&raw);
    uint64_t epoch;
    TimeCivil c;
    uint64_t monoAtRtc = timeMonotonicNs();
    if (timeRtcDecode(&raw, &epoch, &c) == STATUS_OK) {
        /* The RTC counts whole seconds and we read it at a random point inside one: +0.5 s centres
         * the error on zero (about +-0.5 s) instead of always lagging by up to 1 s (D-180). */
        wallBaseNs = epoch * 1000000000ull + 500000000ull - monoAtRtc;
        wallOk = true;
        if (raw.hasCentury) {
            klogWrite(KLOG_INFO, "time",
                      "rtc %04u-%02u-%02uT%02u:%02u:%02uZ %s %s century=0x%02x epoch=%llu", c.year,
                      c.mon, c.day, c.hour, c.min, c.sec, (raw.regB & 4) ? "binary" : "bcd",
                      (raw.regB & 2) ? "24h" : "12h", (unsigned)raw.century,
                      (unsigned long long)epoch);
        } else {
            klogWrite(KLOG_INFO, "time",
                      "rtc %04u-%02u-%02uT%02u:%02u:%02uZ %s %s century=none epoch=%llu", c.year,
                      c.mon, c.day, c.hour, c.min, c.sec, (raw.regB & 4) ? "binary" : "bcd",
                      (raw.regB & 2) ? "24h" : "12h", (unsigned long long)epoch);
        }
    } else {
        wallBaseNs = 0;
        wallOk = false;
        klogWrite(KLOG_WARN, "time", "rtc: unreadable registers; the wall clock is not set");
    }
}

void timerInit(TimerObj *t, TimerFn fn, void *ctx) {
    if (t == NULL || fn == NULL) {
        panicBug("timerInit: NULL timer or callback");
    }
    t->deadlineNs = 0;
    t->seq = 0;
    t->fn = fn;
    t->ctx = ctx;
    /* batchNext is deliberately left alone: the expiry pass owns it while `t` is EXPIRED, and a
     * callback re-initialising a not-yet-run sibling must not cut the rest of the batch off. */
    t->heapIndex = 0;
    t->state = TIMER_IDLE;
}

Status timerArm(TimerObj *t, uint64_t deadlineNs) {
    if (t == NULL || t->fn == NULL || !timersReady) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = archIrqSave();
    TimerCpu *tc = timerCpu();
    TimerObj *oldRoot = timerHeapPeek(&tc->heap);
    if (t->state == TIMER_ARMED) {
        timerHeapRemove(&tc->heap, t);
        t->state = TIMER_IDLE;
    }
    uint64_t oldDeadline = t->deadlineNs;
    t->deadlineNs = deadlineNs;
    Status st = timerHeapInsert(&tc->heap, t);
    if (st != STATUS_OK) {
        t->deadlineNs = oldDeadline; /* the contract: a refused `t` is left unchanged */
    } else if (timerHeapPeek(&tc->heap) != oldRoot || oldRoot == t) {
        reprogram();
    }
    archIrqRestore(f);
    return st;
}

Status timerCancel(TimerObj *t) {
    if (t == NULL) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = archIrqSave();
    TimerCpu *tc = timerCpu();
    Status st = STATUS_ERR_NOT_FOUND;
    if (t->state == TIMER_ARMED) {
        bool wasRoot = timerHeapPeek(&tc->heap) == t;
        timerHeapRemove(&tc->heap, t);
        t->state = TIMER_IDLE;
        if (wasRoot) {
            reprogram();
        }
        st = STATUS_OK;
    } else if (t->state == TIMER_EXPIRED) {
        t->state = TIMER_IDLE; /* the expiry pass skips it */
        st = STATUS_OK;
    }
    archIrqRestore(f);
    return st;
}

bool timerIsArmed(const TimerObj *t) {
    return t != NULL && t->state == TIMER_ARMED;
}

void timeTimerIrq(uint32_t vector, void *ctx) {
    (void)vector;
    (void)ctx;
    TimerCpu *tc = timerCpu();
    tc->expiring = true;
    uint64_t now = timeMonotonicNs();
    /* Collect everything due into a FIFO first, so a callback that re-arms itself with a deadline
     * in the past cannot loop inside one handler. */
    TimerObj *head = NULL, *tail = NULL;
    for (TimerObj *t = timerHeapPeek(&tc->heap); t != NULL && t->deadlineNs <= now;
         t = timerHeapPeek(&tc->heap)) {
        timerHeapRemove(&tc->heap, t);
        t->state = TIMER_EXPIRED;
        t->batchNext = NULL;
        if (tail == NULL) {
            head = t;
        } else {
            tail->batchNext = t;
        }
        tail = t;
    }
    for (TimerObj *t = head; t != NULL;) {
        TimerObj *next = t->batchNext;
        if (t->state == TIMER_EXPIRED) { /* a callback may have cancelled or re-armed it */
            t->state = TIMER_IDLE;
            t->fn(t, t->ctx);
        }
        t = next;
    }
    tc->expiring = false;
    reprogram();
}
