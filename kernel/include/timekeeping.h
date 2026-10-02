/* Kernel time (ARCHITECTURE §7.5, D-176..D-182, ROADMAP M3.3): a monotonic clock from the
 * calibrated TSC, a wall clock from the CMOS RTC, and one-shot timer objects on a per-CPU min-heap
 * driven by the LAPIC timer. The pure arithmetic and heap are time-core.h; the hardware is
 * kernel/arch/x86_64/{clockref,time-x86,rtc}.c behind <arch/timer.h>.
 *
 * Locking: until M3.4 every timer function is an IRQ-disable section on the (only) CPU's queue;
 * M3.5 swaps in a per-CPU spinlock-irqsave. All of them are IRQ-safe (callable from any handler,
 * a timer callback included), never allocate and never sleep. */
#ifndef KERNEL_TIMEKEEPING_H
#define KERNEL_TIMEKEEPING_H

#include "time-core.h"
#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

/* Calibrates the clocks, brings up the LAPIC timer and reads the RTC (logging each, tag "time").
 * Boot-time, BSP, once (a second call panics); IF is already 1 (D-173), after irqInit() and
 * acpiInit(). Panics if the TSC cannot be calibrated or the timer does not count. */
void timeInit(void);

/* Nanoseconds since timeInit() finished calibrating (0 before). Monotonic on one CPU; cross-CPU
 * offsets arrive with M3.5. No locks; IRQ-safe; never sleeps; about 20-30 cycles. */
uint64_t timeMonotonicNs(void);
/* Nanoseconds since 1970-01-01T00:00:00Z: the RTC at boot (assumed UTC, at most 1 s behind: there
 * is no seconds-edge sync) plus the monotonic time since. Meaningful only if timeWallValid(). */
uint64_t timeWallNs(void);
bool timeWallValid(void);
/* The calibrated TSC frequency (0 before timeInit()). */
uint64_t timeTscHz(void);

/* Timer objects live in caller storage and must outlive their arming (a stack object is fine only
 * if cancelled before the frame returns). The callback runs in IRQ context on the arming CPU with
 * IF=0 and irqDepth()==1, after the kernel has popped the timer: no allocation, no sleeping, no
 * klog needed, no ktest assertions (the irq.h handler rules). It may call timerArm()/timerCancel()
 * on any timer, itself included. */
void timerInit(TimerObj *t, TimerFn fn, void *ctx); /* `fn` must not be NULL (panicBug) */

/* Arms `t` to fire at the absolute timeMonotonicNs() value `deadlineNs`, re-arming it if it is
 * already armed or waiting to run. A deadline in the past fires from the timer interrupt as soon
 * as interrupts are on, never synchronously from this call. STATUS_ERR_INVALID: `t` NULL, `fn`
 * unset or before timeInit(). STATUS_ERR_NO_MEMORY: the 256-entry queue is full (`t` unchanged). */
Status timerArm(TimerObj *t, uint64_t deadlineNs);
/* STATUS_OK: `t` was armed (or popped but not started) and its callback will not run.
 * STATUS_ERR_NOT_FOUND: idle, or the callback has already started. STATUS_ERR_INVALID: NULL. */
Status timerCancel(TimerObj *t);
bool timerIsArmed(const TimerObj *t);

/* The timer interrupt handler (an IrqHandler): runs every expired timer, then reprograms the
 * hardware for the next one. Registered by archTimerInit() for vector 0xFE. */
void timeTimerIrq(uint32_t vector, void *ctx);
/* Reprograms the hardware for the earliest deadline (or stops it); for code that has just
 * re-initialized the LAPIC timer registers (lapicTimerCpuSetup()). IF must be 0. */
void timeReprogram(void);

#endif
