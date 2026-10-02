/* Pure timekeeping core (ARCHITECTURE §7.5, D-176..D-180, ROADMAP M3.3): the fixed-point scaling
 * and calibration arithmetic, the CMOS RTC decoder with its civil-date -> epoch conversion, and the
 * per-CPU timer heap. Nothing here touches hardware, klog or the allocators, so it host-tests
 * (tests/host/kernel_time_core_test.c, kernel_timer_heap_test.c) and is shared by the kernel
 * unchanged. The hardware side is kernel/arch/x86_64/{clockref,time-x86,rtc}.c and the portable
 * glue is timekeeping.h.
 *
 * Nothing here divides a 128-bit value: the kernel has no compiler-rt (D-104), so `__udivti3` would
 * be a link error. The only 128-bit operation is the multiply in timeScale(). */
#ifndef KERNEL_TIME_CORE_H
#define KERNEL_TIME_CORE_H

#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

#define TIME_SHIFT 32

/* floor(v * mult / 2^32), the truncated low 64 bits. For the multipliers timeMakeMult() builds this
 * cannot overflow when the true result fits in 64 bits. Pure; no locks; IRQ-safe. */
uint64_t timeScale(uint64_t v, uint64_t mult);

/* *out = floor(to * 2^32 / from), the multiplier that turns `from`-rate ticks into `to`-rate
 * ticks. STATUS_ERR_INVALID if from == 0, to / from >= 2^32 or to % from >= 2^32 (the fraction
 * would overflow). `out` untouched on error. Pure. */
Status timeMakeMult(uint64_t from, uint64_t to, uint64_t *out);

/* *out = ticks * refHz / refTicks: a frequency measured as `ticks` of the unknown clock over
 * `refTicks` of a clock running at `refHz`. STATUS_ERR_INVALID if refTicks == 0 or the product
 * overflows 64 bits. Pure. */
Status timeCalcHz(uint64_t ticks, uint64_t refTicks, uint64_t refHz, uint64_t *out);

/* The median of three values; pure. */
uint64_t timeMedian3(uint64_t a, uint64_t b, uint64_t c);

/* (now - then) & mask: the elapsed ticks of a free-running counter whose width is `mask` + 1
 * (0xFFFFFF for a 24-bit PM timer, 0xFFFFFFFF for a 32-bit counter). Correct across one wrap. */
uint32_t timeRefDelta(uint32_t now, uint32_t then, uint32_t mask);

/* --- calendar / RTC ------------------------------------------------------------------------ */

/* Seconds since 1970-01-01T00:00:00Z for a valid proleptic-Gregorian date with year >= 1970. The
 * fields are NOT range-checked (timeRtcDecode() does that). Pure. */
uint64_t timeCivilToEpoch(uint32_t year, uint32_t mon, uint32_t day, uint32_t hour, uint32_t min,
                          uint32_t sec);
/* Days in `mon` (1-12) of `year`; 0 for an invalid month. Pure. */
uint32_t timeDaysInMonth(uint32_t year, uint32_t mon);

/* The raw CMOS registers (index 0x00 sec, 0x02 min, 0x04 hour, 0x07 day, 0x08 month, 0x09 year,
 * 0x0B status B) plus the FADT century register when there is one. */
typedef struct {
    uint8_t sec, min, hour, day, mon, year, century, regB;
    bool hasCentury;
} TimeRtcRaw;

/* A broken-down UTC date and time. */
typedef struct {
    uint32_t year, mon, day, hour, min, sec;
} TimeCivil;

/* Decodes `raw`: status B bit 2 set means binary, else BCD (a nibble above 9 in a date or time
 * field is INVALID); bit 1 clear means 12-hour mode (hour bit 7 = PM, 12 -> 0, PM adds 12). The
 * year is century * 100 + yy when `hasCentury` and the century byte decodes to 19..29, else
 * 2000 + yy: a century byte that is not valid BCD is ignored like an implausible one (D-180), never
 * INVALID. Every field is range-checked (days per month included) and the year must be >= 1970.
 * STATUS_ERR_INVALID otherwise, `*epoch` untouched. `civil` may be NULL. Pure. */
Status timeRtcDecode(const TimeRtcRaw *raw, uint64_t *epoch, TimeCivil *civil);

/* --- timer heap ---------------------------------------------------------------------------- */

typedef struct TimerObj TimerObj;
typedef void (*TimerFn)(TimerObj *t, void *ctx);

#define TIMER_IDLE    0
#define TIMER_ARMED   1
#define TIMER_EXPIRED 2 /* popped for dispatch, callback not started yet */

struct TimerObj {
    uint64_t deadlineNs; /* absolute timeMonotonicNs() value */
    uint64_t seq;        /* arm order, the tie-break between equal deadlines */
    TimerFn fn;
    void *ctx;
    TimerObj *batchNext; /* the expiry pass's private FIFO link */
    uint32_t heapIndex;  /* valid only while ARMED */
    uint8_t state;
};

#define TIMER_HEAP_CAP 256

typedef struct {
    TimerObj *slot[TIMER_HEAP_CAP];
    uint32_t count;
    uint64_t nextSeq;
} TimerHeap;

/* A binary min-heap ordered by (deadlineNs, seq), so equal deadlines come out in arm order. No
 * locking (the caller provides exclusion), no allocation. */
void timerHeapInit(TimerHeap *h);
/* Inserts an IDLE-or-EXPIRED-not-in-heap `t` (its deadlineNs set), assigning t->seq, setting its
 * state ARMED and heapIndex. STATUS_ERR_NO_MEMORY if the heap is full (`t` unchanged). */
Status timerHeapInsert(TimerHeap *h, TimerObj *t);
/* Removes `t`; precondition: slot[t->heapIndex] == t (t is in this heap). Leaves its state alone.
 */
void timerHeapRemove(TimerHeap *h, TimerObj *t);
/* The earliest timer, or NULL. */
TimerObj *timerHeapPeek(const TimerHeap *h);
/* Removes and returns the earliest timer, or NULL. */
TimerObj *timerHeapPop(TimerHeap *h);
/* Walks the whole heap: true iff the heap order, heapIndex back-pointers and count all hold. For
 * tests. */
bool timerHeapCheck(const TimerHeap *h);

#endif
