/* ACPI PM timer and HPET as calibration references, plus the TSC calibration loop (M3.3,
 * D-177). See clockref.h. */
#include "include/clockref.h"

#include "include/cpu-impl.h"
#include "include/io-impl.h"

#include "acpi.h"
#include "klog.h"
#include "time-core.h"
#include "vmm.h"

#include <stdbool.h>
#include <stdint.h>

#define PM_TIMER_HZ        3579545ull
#define LIVENESS_TSC_LIMIT (1ull << 26)

#define HPET_GCAP_LO     0x000
#define HPET_GCAP_PERIOD 0x004
#define HPET_GEN_CONF    0x010
#define HPET_COUNTER_LO  0x0F0
#define HPET_TN_CONF(n)  (0x100 + 0x20 * (n))
#define HPET_REGS_SIZE   0x500

static ClockRef pmRef, hpetRef;
static bool pmTried, pmOk, hpetTried, hpetOk;

uint32_t clockRefRead(const ClockRef *r) {
    uint32_t v = r->io ? ioInLong(r->port) : *r->mmio;
    return v & r->mask;
}

/* True if the counter changes within LIVENESS_TSC_LIMIT TSC ticks of polling (a stuck or absent
 * counter would otherwise hang calibration). */
static bool refAdvances(const ClockRef *r) {
    uint32_t first = clockRefRead(r);
    uint64_t start = archReadTsc();
    while (archReadTsc() - start < LIVENESS_TSC_LIMIT) {
        if (clockRefRead(r) != first) {
            return true;
        }
        archPause();
    }
    return false;
}

bool clockRefPm(ClockRef *out) {
    if (!pmTried) {
        pmTried = true;
        const AcpiInfo *info = acpiGetInfo();
        if (info == NULL || info->fadtStatus != STATUS_OK || !info->fadt.pmTimerPresent) {
            klogWrite(KLOG_INFO, "time", "pmtimer not found");
        } else {
            const AcpiGas *g = &info->fadt.pmTmr;
            ClockRef r = {.name = "pmtimer", .hz = PM_TIMER_HZ};
            r.mask = info->fadt.pmTimer32Bit ? 0xFFFFFFFFu : 0xFFFFFFu;
            bool ok = false;
            if (g->bitOffset != 0 || (g->accessSize != 0 && g->accessSize != 3)) {
                klogWrite(KLOG_WARN, "time", "pmtimer unusable: GAS bitOffset=%u accessSize=%u",
                          (unsigned)g->bitOffset, (unsigned)g->accessSize);
            } else if (g->spaceId == 1) {
                if (g->address > 0xFFFC) {
                    klogWrite(KLOG_WARN, "time", "pmtimer unusable: port 0x%llx",
                              (unsigned long long)g->address);
                } else {
                    r.io = true;
                    r.port = (uint16_t)g->address;
                    ok = true;
                }
            } else {
                volatile void *va;
                Status st = vmmMapMmio(g->address, 4, &va);
                if (st != STATUS_OK) {
                    klogWrite(KLOG_WARN, "time", "pmtimer unusable: cannot map 0x%llx (status %d)",
                              (unsigned long long)g->address, (int)st);
                } else {
                    r.mmio = (volatile uint32_t *)va;
                    ok = true;
                }
            }
            if (ok && !refAdvances(&r)) {
                klogWrite(KLOG_WARN, "time", "pmtimer unusable: the counter does not advance");
                ok = false;
            }
            if (!ok && !r.io && r.mmio != NULL) {
                vmmUnmapMmio(r.mmio, 4); /* don't keep a mapping of a clock we will not use */
            }
            if (ok) {
                if (r.io) {
                    klogWrite(KLOG_INFO, "time", "pmtimer io=0x%x %u-bit", (unsigned)r.port,
                              info->fadt.pmTimer32Bit ? 32u : 24u);
                } else {
                    klogWrite(KLOG_INFO, "time", "pmtimer mmio=0x%llx %u-bit",
                              (unsigned long long)g->address, info->fadt.pmTimer32Bit ? 32u : 24u);
                }
                pmRef = r;
                pmOk = true;
            }
        }
    }
    if (pmOk && out != NULL) {
        *out = pmRef;
    }
    return pmOk;
}

static uint32_t hpetRd(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void hpetWr(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

bool clockRefHpet(ClockRef *out) {
    if (!hpetTried) {
        hpetTried = true;
        const AcpiInfo *info = acpiGetInfo();
        if (info == NULL || info->hpetStatus != STATUS_OK || info->hpet.base == 0) {
            klogWrite(KLOG_INFO, "time", "hpet not found");
        } else {
            volatile void *va;
            Status st = vmmMapMmio(info->hpet.base, HPET_REGS_SIZE, &va);
            if (st != STATUS_OK) {
                klogWrite(KLOG_WARN, "time", "hpet unusable: cannot map 0x%llx (status %d)",
                          (unsigned long long)info->hpet.base, (int)st);
            } else {
                volatile uint8_t *base = (volatile uint8_t *)va;
                uint32_t gcap = hpetRd(base, HPET_GCAP_LO);
                uint32_t period = hpetRd(base, HPET_GCAP_PERIOD); /* femtoseconds per tick */
                uint32_t timers = ((gcap >> 8) & 0x1Fu) + 1;
                if ((gcap & 0xFFu) == 0 || period == 0 || period > 0x05F5E100u) {
                    klogWrite(KLOG_WARN, "time", "hpet unusable: GCAP=0x%x period=%ufs",
                              (unsigned)gcap, (unsigned)period);
                    vmmUnmapMmio(va, HPET_REGS_SIZE);
                } else {
                    /* No comparator may interrupt (Tn_INT_ENB_CNF = bit 2), ENABLE the counter,
                     * and leave LEG_RT clear so the PIT keeps ISA IRQ 0 (M3.2's PIT ktest). */
                    for (uint32_t n = 0; n < timers; n++) {
                        uint32_t conf = hpetRd(base, HPET_TN_CONF(n));
                        hpetWr(base, HPET_TN_CONF(n), conf & ~(1u << 2));
                    }
                    uint32_t gen = hpetRd(base, HPET_GEN_CONF);
                    hpetWr(base, HPET_GEN_CONF, (gen & ~3u) | 1u);
                    ClockRef r = {.name = "hpet",
                                  .hz = 1000000000000000ull / period,
                                  .mask = 0xFFFFFFFFu,
                                  .mmio = (volatile uint32_t *)(base + HPET_COUNTER_LO)};
                    if (!refAdvances(&r)) {
                        klogWrite(KLOG_WARN, "time", "hpet unusable: the counter does not advance");
                        vmmUnmapMmio(va, HPET_REGS_SIZE);
                    } else {
                        klogWrite(KLOG_INFO, "time",
                                  "hpet base=0x%016llx period=%ufs freq=%lluHz timers=%u "
                                  "counter=%u-bit",
                                  (unsigned long long)info->hpet.base, (unsigned)period,
                                  (unsigned long long)r.hz, (unsigned)timers,
                                  ((gcap >> 13) & 1u) ? 64u : 32u);
                        hpetRef = r;
                        hpetOk = true;
                    }
                }
            }
        }
    }
    if (hpetOk && out != NULL) {
        *out = hpetRef;
    }
    return hpetOk;
}

#define CAL_WINDOWS        3
#define CAL_REF_DIV        20 /* window = hz / 20 ticks = 50 ms */
#define CAL_MAX_SPREAD_PPM 1000u
#define CAL_STUCK_TSC      (1ull << 34)

/* One window; returns the measured TSC Hz, or 0 if the arithmetic fails or the reference is stuck.
 * IRQs are off across the measurement so an interrupt cannot stretch a bracket. */
static uint64_t calibrateWindow(const ClockRef *ref) {
    uint64_t target = ref->hz / CAL_REF_DIV;
    uint64_t f = archIrqSave();
    uint64_t a0 = archReadTscOrdered();
    uint32_t r0 = clockRefRead(ref);
    uint64_t a1 = archReadTscOrdered();
    uint64_t b0, b1;
    uint32_t r1;
    for (;;) {
        archPause();
        b0 = archReadTscOrdered();
        r1 = clockRefRead(ref);
        b1 = archReadTscOrdered();
        if (timeRefDelta(r1, r0, ref->mask) >= target) {
            break;
        }
        if (b0 - a0 > CAL_STUCK_TSC) {
            archIrqRestore(f);
            return 0;
        }
    }
    archIrqRestore(f);
    uint64_t tscTicks = (b0 + (b1 - b0) / 2) - (a0 + (a1 - a0) / 2);
    uint64_t hz;
    if (timeCalcHz(tscTicks, timeRefDelta(r1, r0, ref->mask), ref->hz, &hz) != STATUS_OK) {
        return 0;
    }
    return hz;
}

static Status calibrateThree(const ClockRef *ref, uint64_t *median, uint32_t *spreadPpm) {
    uint64_t v[CAL_WINDOWS];
    for (int i = 0; i < CAL_WINDOWS; i++) {
        v[i] = calibrateWindow(ref);
        if (v[i] == 0) {
            return STATUS_ERR_INVALID;
        }
    }
    uint64_t med = timeMedian3(v[0], v[1], v[2]);
    uint64_t lo = v[0], hi = v[0];
    for (int i = 1; i < CAL_WINDOWS; i++) {
        lo = v[i] < lo ? v[i] : lo;
        hi = v[i] > hi ? v[i] : hi;
    }
    *median = med;
    *spreadPpm = (uint32_t)(((hi - lo) * 1000000ull) / med);
    return STATUS_OK;
}

Status tscCalibrate(const ClockRef *ref, uint64_t *hz, uint32_t *spreadPpm) {
    uint64_t med;
    uint32_t spread;
    Status st = calibrateThree(ref, &med, &spread);
    if (st != STATUS_OK) {
        return st;
    }
    if (spread > CAL_MAX_SPREAD_PPM) {
        st = calibrateThree(ref, &med, &spread);
        if (st != STATUS_OK) {
            return st;
        }
    }
    *hz = med;
    *spreadPpm = spread;
    return STATUS_OK;
}
