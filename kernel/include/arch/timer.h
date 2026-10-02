/* Portable-facing clock and clock-event interface (ARCHITECTURE §2/§7.5, D-176..D-180), implemented
 * for x86_64 by kernel/arch/x86_64/{time-x86,rtc}.c and used by kernel/core/timekeeping.c. */
#ifndef KERNEL_INCLUDE_ARCH_TIMER_H
#define KERNEL_INCLUDE_ARCH_TIMER_H

#include "time-core.h"

#include <stdint.h>

/* Applies the TSC policy (D-178), finds the calibration reference (D-177) and measures the TSC
 * frequency, logging each step; `*tscHz` receives it. Panics if the TSC is not invariant on bare
 * metal, or if neither the ACPI PM timer nor an HPET is usable. Boot-time, BSP, once; needs
 * acpiInit() and vmmInit(); IF may be 1. May not sleep. */
void archClockInit(uint64_t *tscHz);

/* Registers the timer interrupt handler for vector 0xFE, calibrates the LAPIC timer against the
 * TSC, and programs the LVT (TSC-deadline when CPUID allows, else one-shot). Boot-time, BSP,
 * once, after archClockInit() and irqInit(); the handler runs timeTimerIrq(). */
void archTimerInit(uint64_t tscHz);

/* Arms the timer to fire in `deltaNs` nanoseconds (0 = as soon as possible; the caller clamps to
 * at most 2^40) or stops it. The hardware may fire early (the handler re-checks); it never fires
 * twice for one call. IF must be 0 (the callers hold an IRQ-disable section). No other locks;
 * may not sleep. */
void archTimerSet(uint64_t deltaNs);
void archTimerStop(void);

/* Reads the CMOS RTC registers into `*out` (D-180): waits (bounded) out an update in progress and
 * re-reads until two consecutive snapshots agree. Needs timeInit()'s monotonic clock. Not from IRQ
 * context; may not sleep. */
void archRtcRead(TimeRtcRaw *out);

#endif
