/* ktests for the x86 clock hardware (M3.3, D-177..D-182, ROADMAP M3.3): the 100 ms one-shot timer
 * measured against an independent clock (the ACPI PM timer), the TSC against both the PM timer and
 * the HPET, the CPUID-driven policy choices, and the CMOS RTC against the wall clock. The queue
 * semantics are in kernel/test/time_test.c. As there, TimerObjs are static, callbacks only record,
 * and waits poll with archPause(). */
#include "apic.h"
#include "clockref.h"

#include "irq.h"
#include "klog.h"
#include "ktest.h"
#include "time-core.h"
#include "timekeeping.h"

#include <arch/cpu.h>
#include <arch/timer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MS 1000000ull

typedef struct {
    volatile uint32_t count;
    volatile uint64_t firedAt;
    volatile uint32_t firedRef;
    const ClockRef *ref;
} Shot;

static void shotCb(TimerObj *t, void *ctx) {
    (void)t;
    Shot *s = (Shot *)ctx;
    s->firedAt = timeMonotonicNs();
    s->firedRef = clockRefRead(s->ref);
    s->count++;
}

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

/* D-182: a 100 ms timer must fire in [deadline, deadline + 5 ms] by the TSC clock AND take 95-105
 * ms by the PM timer, an independent clock, so a wrong calibration cannot cancel out. Up to three
 * attempts absorb host scheduling noise (QEMU on a loaded machine); an early fire (a calibration
 * or programming error, never noise) fails at once. */
KTEST(time_oneshot_100ms) {
    static Shot s;
    static TimerObj t;
    ClockRef ref;
    KTEST_ASSERT(clockRefPm(&ref));
    s.ref = &ref;
    bool pass = false;
    for (int attempt = 1; attempt <= 3 && !pass; attempt++) {
        s.count = 0;
        timerInit(&t, shotCb, &s);
        uint32_t ref0 = clockRefRead(&ref);
        uint64_t deadline = timeMonotonicNs() + 100 * MS;
        KTEST_ASSERT(timerArm(&t, deadline) == STATUS_OK);
        bool fired = waitCount(&s.count, 1, 1000 * MS);
        if (!fired) {
            (void)timerCancel(&t);
            KTEST_ASSERT(fired);
        }
        uint64_t firedAt = s.firedAt;
        uint32_t ticks = timeRefDelta(s.firedRef, ref0, ref.mask);
        uint64_t pmNs = (uint64_t)ticks * 1000000000ull / ref.hz;
        KTEST_ASSERT(firedAt >= deadline); /* early: a real bug, no retry */
        uint64_t late = firedAt - deadline;
        klogWrite(KLOG_INFO, "time", "oneshot-100ms attempt %d: late=%lluus pm=%lluus", attempt,
                  (unsigned long long)(late / 1000), (unsigned long long)(pmNs / 1000));
        pass = late <= 5 * MS && pmNs >= 95 * MS && pmNs <= 105 * MS;
    }
    KTEST_ASSERT(pass);
}

KTEST(time_tsc_invariance_matches_cpuid) {
    uint32_t r[4];
    archCpuid(0x80000000u, 0, r);
    bool cpuidInvariant = false;
    if (r[0] >= 0x80000007u) {
        archCpuid(0x80000007u, 0, r);
        cpuidInvariant = ((r[3] >> 8) & 1u) != 0;
    }
    KTEST_ASSERT(archTscInvariant() == cpuidInvariant);
    archCpuid(1, 0, r);
    bool hypervisor = ((r[2] >> 31) & 1u) != 0;
    KTEST_ASSERT(hypervisor || archTscInvariant()); /* bare metal without it panics at boot */
    KTEST_ASSERT(timeTscHz() >= 100000000ull && timeTscHz() <= 10000000000ull);
}

KTEST(time_tsc_matches_pmtimer) {
    ClockRef ref;
    KTEST_ASSERT(clockRefPm(&ref));
    uint64_t target = ref.hz / 5; /* 200 ms of PM ticks */
    uint64_t f = archIrqSave();
    uint64_t m0 = timeMonotonicNs();
    uint32_t r0 = clockRefRead(&ref);
    uint32_t r1;
    do {
        archPause();
        r1 = clockRefRead(&ref);
    } while (timeRefDelta(r1, r0, ref.mask) < target);
    uint64_t m1 = timeMonotonicNs();
    archIrqRestore(f);
    uint64_t refNs = (uint64_t)timeRefDelta(r1, r0, ref.mask) * 1000000000ull / ref.hz;
    uint64_t monoNs = m1 - m0;
    uint64_t diff = monoNs > refNs ? monoNs - refNs : refNs - monoNs;
    KTEST_ASSERT(diff * 200 <= refNs); /* within 0.5% */
}

KTEST(time_hpet_calibration_agrees) {
    ClockRef hpet;
    KTEST_ASSERT(clockRefHpet(&hpet)); /* q35 always has one; the ACPI dump check requires it */
    uint64_t hz;
    uint32_t spread;
    KTEST_ASSERT(tscCalibrate(&hpet, &hz, &spread) == STATUS_OK);
    uint64_t boot = timeTscHz();
    uint64_t diff = hz > boot ? hz - boot : boot - hz;
    KTEST_ASSERT(diff * 500 <= boot); /* within 0.2% */
}

KTEST(time_lapic_timer_mode_matches_cpuid) {
    uint32_t r[4];
    archCpuid(1, 0, r);
    bool deadline = ((r[2] >> 24) & 1u) != 0;
    KTEST_ASSERT(lapicTimerDeadlineMode() == deadline);
    uint32_t lvt = lapicRead(LAPIC_REG_TIMER);
    KTEST_ASSERT_EQ(lvt & 0xFFu, LAPIC_TIMER_VECTOR);
    KTEST_ASSERT((lvt & LAPIC_LVT_MASKED) == 0);
    KTEST_ASSERT_EQ(lvt & (3u << 17), deadline ? LAPIC_LVT_TIMER_TSCDL : 0);
    KTEST_ASSERT(lapicTimerHz() >= 1000000ull);
}

KTEST(time_wall_matches_rtc) {
    KTEST_ASSERT(timeWallValid());
    TimeRtcRaw raw;
    archRtcRead(&raw);
    uint64_t epoch;
    KTEST_ASSERT(timeRtcDecode(&raw, &epoch, NULL) == STATUS_OK);
    uint64_t wallNs = timeWallNs();
    uint64_t wallSec = wallNs / 1000000000ull;
    uint64_t diff = wallSec > epoch ? wallSec - epoch : epoch - wallSec;
    KTEST_ASSERT(diff <= 2); /* boot-time RTC read lags by up to 1 s (no seconds-edge sync) */
    KTEST_ASSERT(epoch >= 1704067200ull && epoch < 4102444800ull); /* 2024 .. 2100 */
    /* Roadmap clause 3: tests/harness/run-qemu.sh timestamps this line against the host's clock
     * (D-181) and mk/test.mk requires the two to agree within 2 s. */
    klogWrite(KLOG_INFO, "time", "wall-check epoch=%llu.%09llu", (unsigned long long)wallSec,
              (unsigned long long)(wallNs % 1000000000ull));
}

/* --- bug-sweeper (M3.3 finish): adversarial hardware-facing tests --------------------------- */

/* timerArm()/timerCancel() from another interrupt handler (IRQ context, not the timer's own): the
 * arm takes effect (it reprograms, since only the timer handler defers that) and fires once. */
typedef struct {
    TimerObj *t;
    volatile uint64_t deadline;
    volatile int32_t armSt, cancelSt;
    volatile uint32_t ran;
    volatile uint32_t doCancel;
} ArmFromIrq;

static ArmFromIrq armIrq;
static volatile uint32_t armIrqFired;
static volatile uint64_t armIrqFiredAt;

static void armIrqTimerCb(TimerObj *t, void *ctx) {
    (void)t;
    (void)ctx;
    armIrqFiredAt = timeMonotonicNs();
    armIrqFired++;
}

static void armIrqHandler(uint32_t vector, void *ctx) {
    (void)vector;
    ArmFromIrq *a = (ArmFromIrq *)ctx;
    if (a->doCancel) {
        a->cancelSt = (int32_t)timerCancel(a->t);
    } else {
        a->deadline = timeMonotonicNs() + 10 * MS;
        a->armSt = (int32_t)timerArm(a->t, a->deadline);
    }
    a->ran++;
}

KTEST(time_timer_arm_from_other_irq) {
    static TimerObj t;
    uint32_t vec;
    KTEST_ASSERT(irqAllocVector(&vec) == STATUS_OK);
    armIrq.t = &t;
    armIrq.armSt = armIrq.cancelSt = 99;
    armIrq.ran = 0;
    armIrq.doCancel = 0;
    armIrqFired = 0;
    timerInit(&t, armIrqTimerCb, NULL);
    Status reg = irqRegister(vec, armIrqHandler, &armIrq);
    lapicSendSelfIpi((uint8_t)vec);
    bool ran = waitCount(&armIrq.ran, 1, 200 * MS);
    bool fired = waitCount(&armIrqFired, 1, 500 * MS);
    uint64_t firedAt = armIrqFiredAt;
    /* now arm far away from thread context and cancel it from the handler */
    Status farArm = timerArm(&t, timeMonotonicNs() + 200 * MS);
    armIrq.doCancel = 1;
    lapicSendSelfIpi((uint8_t)vec);
    bool ran2 = waitCount(&armIrq.ran, 2, 200 * MS);
    uint32_t before = armIrqFired;
    uint64_t start = timeMonotonicNs();
    while (timeMonotonicNs() - start < 300 * MS) {
        archPause();
    }
    uint32_t after = armIrqFired;
    (void)timerCancel(&t);
    (void)irqUnregister(vec);
    (void)irqFreeVector(vec);
    KTEST_ASSERT(reg == STATUS_OK);
    KTEST_ASSERT(ran && ran2);
    KTEST_ASSERT_EQ(armIrq.armSt, STATUS_OK);
    KTEST_ASSERT(fired);
    KTEST_ASSERT(firedAt >= armIrq.deadline);
    KTEST_ASSERT(farArm == STATUS_OK);
    KTEST_ASSERT_EQ(armIrq.cancelSt, STATUS_OK);
    KTEST_ASSERT_EQ(after, before); /* the cancelled timer never fired */
    KTEST_ASSERT_EQ(after, 1);
}

/* The hardware is always programmed for the queue's root: arming an earlier timer shortens the
 * programmed interval, cancelling the root lengthens it to the next one, and an empty queue stops
 * the timer. In one-shot mode the programmed count is the LAPIC initial-count register (and the
 * 2^32-1 clamp of a 2^40 ns delta is visible there); in TSC-deadline mode the IA32_TSC_DEADLINE
 * MSR. Runs with IRQs off so no expiry interferes; every timer is far in the future. */
static uint64_t programmedNs(void) {
    if (lapicTimerDeadlineMode()) {
        uint64_t dl = archRdmsr(MSR_IA32_TSC_DEADLINE);
        uint64_t now = archReadTscOrdered();
        if (dl == 0) {
            return 0;
        }
        return dl <= now ? 1 : (dl - now) * 1000000000ull / timeTscHz();
    }
    uint32_t init = lapicRead(LAPIC_REG_TIMER_INIT);
    return (uint64_t)init * 1000000000ull / lapicTimerHz();
}

KTEST(time_timer_hw_tracks_root) {
    static TimerObj ta, tb, tfar;
    timerInit(&ta, armIrqTimerCb, NULL);
    timerInit(&tb, armIrqTimerCb, NULL);
    timerInit(&tfar, armIrqTimerCb, NULL);
    uint64_t f = archIrqSave();
    uint64_t now = timeMonotonicNs();
    Status s1 = timerArm(&ta, now + 5000 * MS);
    uint64_t pA = programmedNs();
    Status s2 = timerArm(&tb, now + 2000 * MS); /* new earlier root */
    uint64_t pB = programmedNs();
    Status s3 = timerArm(&tb, now + 8000 * MS); /* the root moves behind ta */
    uint64_t pB2 = programmedNs();
    Status s4 = timerArm(&tb, now + 2000 * MS);
    Status c1 = timerCancel(&tb); /* the root goes: back to ta */
    uint64_t pA2 = programmedNs();
    Status c2 = timerCancel(&ta); /* empty: stopped */
    uint64_t pNone = programmedNs();
    Status s5 = timerArm(&tfar, UINT64_MAX); /* clamped to 2^40 ns (and 2^32-1 counts) */
    uint64_t pFar = programmedNs();
    uint32_t farInit = lapicRead(LAPIC_REG_TIMER_INIT);
    Status c3 = timerCancel(&tfar);
    archIrqRestore(f);
    klogWrite(KLOG_INFO, "time", "hw-tracks-root A=%lluus B=%lluus B2=%lluus A2=%lluus far=%llums",
              (unsigned long long)(pA / 1000), (unsigned long long)(pB / 1000),
              (unsigned long long)(pB2 / 1000), (unsigned long long)(pA2 / 1000),
              (unsigned long long)(pFar / MS));
    KTEST_ASSERT(s1 == STATUS_OK && s2 == STATUS_OK && s3 == STATUS_OK && s4 == STATUS_OK &&
                 s5 == STATUS_OK);
    KTEST_ASSERT(c1 == STATUS_OK && c2 == STATUS_OK && c3 == STATUS_OK);
    /* generous windows: QEMU TCG time passes between the arm and the read */
    KTEST_ASSERT(pA > 4500 * MS && pA <= 5001 * MS);
    KTEST_ASSERT(pB > 1500 * MS && pB <= 2001 * MS);
    KTEST_ASSERT(pB2 > 4500 * MS && pB2 <= 5001 * MS); /* ta is the root again */
    KTEST_ASSERT(pA2 > 4500 * MS && pA2 <= 5001 * MS);
    KTEST_ASSERT_EQ(pNone, 0);
    if (lapicTimerDeadlineMode()) {
        KTEST_ASSERT(pFar > (1ull << 40) - 1000 * MS && pFar <= (1ull << 40) + MS);
    } else {
        KTEST_ASSERT_EQ(farInit, 0xFFFFFFFFu);
    }
}

/* archTimerSet() directly: 0 means "as soon as possible" (a count of 1, never 0, which would stop
 * a one-shot timer), 1 ms is about lapicHz/1000 counts, and a 2^40 ns delta clamps to 2^32-1. */
KTEST(time_lapic_oneshot_count_edges) {
    if (lapicTimerDeadlineMode()) {
        uint64_t f = archIrqSave();
        uint64_t t0 = archReadTscOrdered();
        archTimerSet(0);
        uint64_t dl0 = archRdmsr(MSR_IA32_TSC_DEADLINE);
        archTimerSet(1ull << 40);
        uint64_t dlFar = archRdmsr(MSR_IA32_TSC_DEADLINE);
        archTimerStop();
        uint64_t dlStop = archRdmsr(MSR_IA32_TSC_DEADLINE);
        timeReprogram();
        archIrqRestore(f);
        KTEST_ASSERT(dl0 > t0 || dl0 == 0); /* 0: it already fired and the MSR cleared */
        KTEST_ASSERT(dlFar > t0);
        KTEST_ASSERT_EQ(dlStop, 0);
        return;
    }
    uint64_t hz = lapicTimerHz();
    uint64_t f = archIrqSave();
    archTimerSet(0);
    uint32_t c0 = lapicRead(LAPIC_REG_TIMER_INIT);
    archTimerSet(MS);
    uint32_t c1ms = lapicRead(LAPIC_REG_TIMER_INIT);
    archTimerSet(1ull << 40);
    uint32_t cFar = lapicRead(LAPIC_REG_TIMER_INIT);
    archTimerSet((0xFFFFFFFFull * 1000000000ull) / hz - 1000); /* just under the clamp */
    uint32_t cEdge = lapicRead(LAPIC_REG_TIMER_INIT);
    archTimerStop();
    uint32_t cStop = lapicRead(LAPIC_REG_TIMER_INIT);
    timeReprogram();
    archIrqRestore(f);
    KTEST_ASSERT_EQ(c0, 1);
    uint64_t want = hz / 1000;
    KTEST_ASSERT(c1ms >= want && c1ms <= want + 2);
    KTEST_ASSERT_EQ(cFar, 0xFFFFFFFFu);
    KTEST_ASSERT(cEdge < 0xFFFFFFFFu && cEdge > 0xFFFFFFFFu - hz / 1000);
    KTEST_ASSERT_EQ(cStop, 0);
}
