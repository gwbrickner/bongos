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
