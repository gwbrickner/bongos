/* TSC policy and calibration, and the LAPIC timer as the clock-event device (M3.3, D-177..D-179;
 * ARCHITECTURE §7.5). The arithmetic is in kernel/core/time-core.c, the queue and the interrupt
 * handler's expiry loop in kernel/core/timekeeping.c. */
#include "include/apic.h"
#include "include/clockref.h"
#include "include/cpu-impl.h"

#include "irq.h"
#include "klog.h"
#include "panic.h"
#include "time-core.h"
#include "timekeeping.h"

#include <arch/timer.h>
#include <stdbool.h>
#include <stdint.h>

#define LAPIC_DIVIDE_BY_16 0x3u
#define LAPIC_CAL_WINDOWS  3
#define LAPIC_CAL_DIV      50 /* 20 ms of TSC per window */

static bool tscInvariant, hypervisor, tscDeadlineSupported, arat;
static uint64_t tscHzValue;
static uint64_t lapicHz;       /* LAPIC timer counts per second at divide 16 */
static uint64_t nsToTscMult;   /* ns -> TSC ticks */
static uint64_t nsToLapicMult; /* ns -> LAPIC timer counts */
static bool deadlineMode;
static bool timerReady;

uint64_t lapicTimerHz(void) {
    return lapicHz;
}

bool lapicTimerDeadlineMode(void) {
    return deadlineMode;
}

bool archTscInvariant(void) {
    return tscInvariant;
}

static const char *yesNo(bool b) {
    return b ? "yes" : "no";
}

void archClockInit(uint64_t *tscHz) {
    uint32_t r[4];
    archCpuid(0x80000000u, 0, r);
    uint32_t maxExt = r[0];
    tscInvariant = false;
    if (maxExt >= 0x80000007u) {
        archCpuid(0x80000007u, 0, r);
        tscInvariant = ((r[3] >> 8) & 1u) != 0;
    }
    archCpuid(1, 0, r);
    hypervisor = ((r[2] >> 31) & 1u) != 0;
    tscDeadlineSupported = ((r[2] >> 24) & 1u) != 0;
    archCpuid(6, 0, r);
    arat = (r[0] & 4u) != 0;
    klogWrite(KLOG_INFO, "time", "tsc invariant=%s hypervisor=%s tsc-deadline=%s arat=%s",
              yesNo(tscInvariant), yesNo(hypervisor), yesNo(tscDeadlineSupported), yesNo(arat));
    if (!tscInvariant) {
        if (!hypervisor) {
            panic("time: invariant TSC required (CPUID.80000007H:EDX[8]=0)");
        }
        klogWrite(KLOG_WARN, "time",
                  "TSC is not invariant (CPUID.80000007H:EDX[8]=0); hypervisor, continuing");
    }

    ClockRef ref, hpet;
    bool havePm = clockRefPm(&ref);
    bool haveHpet = clockRefHpet(&hpet); /* optional: initialised and logged either way */
    if (!havePm) {
        if (!haveHpet) {
            panic("time: no TSC calibration reference (no ACPI PM timer, no HPET)");
        }
        ref = hpet;
    }

    uint64_t hz;
    uint32_t spread;
    if (tscCalibrate(&ref, &hz, &spread) != STATUS_OK || hz < 1000000ull) {
        panic("time: TSC calibration against %s failed", ref.name);
    }
    klogWrite(KLOG_INFO, "time",
              "tsc %llu Hz (calibrated against %s, median of 3 windows, spread %u ppm)",
              (unsigned long long)hz, ref.name, (unsigned)spread);
    tscHzValue = hz;
    *tscHz = hz;
}

/* One 20 ms window: the LAPIC timer (masked, divide 16) counts down from 0xFFFFFFFF while the TSC
 * runs, bracketed by ordered reads. Returns the LAPIC Hz or 0. */
static uint64_t lapicCalWindow(uint64_t tscHz) {
    uint64_t span = tscHz / LAPIC_CAL_DIV;
    uint64_t f = archIrqSave();
    uint64_t a0 = archReadTscOrdered();
    lapicWrite(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
    uint64_t a1 = archReadTscOrdered();
    uint64_t b0, b1;
    uint32_t cur;
    for (;;) {
        archPause();
        b0 = archReadTscOrdered();
        cur = lapicRead(LAPIC_REG_TIMER_CUR);
        b1 = archReadTscOrdered();
        if (b0 - a1 >= span) {
            break;
        }
    }
    lapicWrite(LAPIC_REG_TIMER_INIT, 0);
    archIrqRestore(f);
    uint64_t counts = 0xFFFFFFFFull - cur;
    uint64_t tscTicks = (b0 + (b1 - b0) / 2) - (a0 + (a1 - a0) / 2);
    uint64_t hz;
    if (counts == 0 || timeCalcHz(counts, tscTicks, tscHz, &hz) != STATUS_OK) {
        return 0;
    }
    return hz;
}

void lapicTimerCpuSetup(void) {
    if (!timerReady) {
        return;
    }
    lapicWrite(LAPIC_REG_TIMER_INIT, 0);
    lapicWrite(LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16);
    if (deadlineMode) {
        lapicWrite(LAPIC_REG_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_TIMER_TSCDL);
        /* SDM Vol 3A §10.5.4.1: the LVT write (an MMIO store in xAPIC mode) must be ordered
         * before the IA32_TSC_DEADLINE WRMSR. Harmless in x2APIC mode. */
        archMfence();
        archWrmsr(MSR_IA32_TSC_DEADLINE, 0);
    } else {
        lapicWrite(LAPIC_REG_TIMER, LAPIC_TIMER_VECTOR); /* one-shot, unmasked */
    }
}

void archTimerInit(uint64_t tscHz) {
    Status st = irqRegister(LAPIC_TIMER_VECTOR, timeTimerIrq, NULL);
    if (st != STATUS_OK) {
        panic("time: cannot register the LAPIC timer vector (status %d)", (int)st);
    }
    /* Calibrate always, even when TSC-deadline is used, so the one-shot path is measured too.
     * A masked timer still counts (SDM Vol 3A §10.5.4). */
    lapicWrite(LAPIC_REG_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR);
    lapicWrite(LAPIC_REG_TIMER_DIV, LAPIC_DIVIDE_BY_16);
    uint64_t v[LAPIC_CAL_WINDOWS];
    for (int i = 0; i < LAPIC_CAL_WINDOWS; i++) {
        v[i] = lapicCalWindow(tscHz);
        if (v[i] == 0) {
            panic("time: the LAPIC timer does not count");
        }
    }
    lapicHz = timeMedian3(v[0], v[1], v[2]);
    if (timeMakeMult(1000000000ull, tscHz, &nsToTscMult) != STATUS_OK ||
        timeMakeMult(1000000000ull, lapicHz, &nsToLapicMult) != STATUS_OK) {
        panic("time: cannot scale ns to ticks (tsc %llu Hz, lapic %llu Hz)",
              (unsigned long long)tscHz, (unsigned long long)lapicHz);
    }
    deadlineMode = tscDeadlineSupported;
    timerReady = true;
    lapicTimerCpuSetup();
    klogWrite(KLOG_INFO, "time", "lapic timer mode=%s freq=%lluHz divide=16 vector=0xfe",
              deadlineMode ? "tsc-deadline" : "oneshot", (unsigned long long)lapicHz);
}

void archTimerSet(uint64_t deltaNs) {
    if (deadlineMode) {
        archWrmsr(MSR_IA32_TSC_DEADLINE,
                  archReadTscOrdered() + timeScale(deltaNs, nsToTscMult) + 1);
        return;
    }
    uint64_t count = timeScale(deltaNs, nsToLapicMult) + 1; /* 0 would stop the timer */
    if (count > 0xFFFFFFFFull) {
        count = 0xFFFFFFFFull;
    }
    lapicWrite(LAPIC_REG_TIMER_INIT, (uint32_t)count);
}

void archTimerStop(void) {
    if (deadlineMode) {
        archWrmsr(MSR_IA32_TSC_DEADLINE, 0);
    } else {
        lapicWrite(LAPIC_REG_TIMER_INIT, 0);
    }
}
