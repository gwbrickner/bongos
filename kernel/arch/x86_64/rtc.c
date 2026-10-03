/* CMOS RTC reader (M3.3, D-180). Ports 0x70/0x71; register layout per the MC146818 datasheet.
 * Never writes status B and never enables RTC interrupts. */
#include "include/io-impl.h"

#include "acpi.h"
#include "klog.h"
#include "spinlock.h"
#include "timekeeping.h"

#include <arch/cpu.h>
#include <arch/timer.h>
#include <stdbool.h>
#include <stdint.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SEC   0x00
#define RTC_MIN   0x02
#define RTC_HOUR  0x04
#define RTC_DAY   0x07
#define RTC_MON   0x08
#define RTC_YEAR  0x09
#define RTC_REG_A 0x0A
#define RTC_REG_B 0x0B

#define RTC_UIP_TIMEOUT_NS 20000000ull
#define RTC_MAX_TRIES      5

/* One lock for the index/data pair (D-201): two CPUs must not interleave them. */
static Spinlock rtcLockObj = SPINLOCK_INIT("rtc");

/* Index (bit 7 clear: NMI stays enabled) then data, as one locked pair. */
static uint8_t cmosRead(uint8_t idx) {
    uint64_t f = spinLockIrqSave(&rtcLockObj);
    ioOutByte(CMOS_INDEX, idx);
    uint8_t v = ioInByte(CMOS_DATA);
    spinUnlockIrqRestore(&rtcLockObj, f);
    return v;
}

/* The FADT century register, or 0 if the platform has none (a valid one is above 0x0D and below
 * 0x80, which keeps it clear of the clock registers and the NMI bit). */
static uint8_t centuryIndex(void) {
    const AcpiInfo *info = acpiGetInfo();
    if (info == NULL || info->fadtStatus != STATUS_OK) {
        return 0;
    }
    uint8_t c = info->fadt.century;
    return (c > 0x0D && c < 0x80) ? c : 0;
}

static void waitNoUpdate(void) {
    uint64_t start = timeMonotonicNs();
    while ((cmosRead(RTC_REG_A) & 0x80u) != 0) {
        if (timeMonotonicNs() - start > RTC_UIP_TIMEOUT_NS) {
            klogWrite(KLOG_WARN, "time", "rtc: update-in-progress did not clear");
            return;
        }
        archPause();
    }
}

static void snapshot(TimeRtcRaw *r, uint8_t centIdx) {
    waitNoUpdate();
    r->sec = cmosRead(RTC_SEC);
    r->min = cmosRead(RTC_MIN);
    r->hour = cmosRead(RTC_HOUR);
    r->day = cmosRead(RTC_DAY);
    r->mon = cmosRead(RTC_MON);
    r->year = cmosRead(RTC_YEAR);
    r->regB = cmosRead(RTC_REG_B);
    r->hasCentury = centIdx != 0;
    r->century = centIdx != 0 ? cmosRead(centIdx) : 0;
}

static bool sameSnapshot(const TimeRtcRaw *a, const TimeRtcRaw *b) {
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour && a->day == b->day &&
           a->mon == b->mon && a->year == b->year && a->century == b->century && a->regB == b->regB;
}

void archRtcRead(TimeRtcRaw *out) {
    uint8_t centIdx = centuryIndex();
    TimeRtcRaw prev, cur;
    snapshot(&prev, centIdx);
    for (int i = 1; i < RTC_MAX_TRIES; i++) {
        snapshot(&cur, centIdx);
        if (sameSnapshot(&prev, &cur)) {
            *out = cur;
            return;
        }
        prev = cur;
    }
    klogWrite(KLOG_WARN, "time", "rtc: no two consecutive reads agreed; using the last");
    *out = prev;
}
