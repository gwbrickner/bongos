/* Pure timekeeping core: see kernel/include/time-core.h. */
#include "time-core.h"

#include <stddef.h>

uint64_t timeScale(uint64_t v, uint64_t mult) {
    return (uint64_t)(((unsigned __int128)v * mult) >> TIME_SHIFT);
}

Status timeMakeMult(uint64_t from, uint64_t to, uint64_t *out) {
    if (from == 0) {
        return STATUS_ERR_INVALID;
    }
    uint64_t whole = to / from;
    uint64_t rem = to % from;
    if (whole >= ((uint64_t)1 << 32) || rem >= ((uint64_t)1 << 32)) {
        return STATUS_ERR_INVALID;
    }
    *out = (whole << 32) + ((rem << 32) / from);
    return STATUS_OK;
}

Status timeCalcHz(uint64_t ticks, uint64_t refTicks, uint64_t refHz, uint64_t *out) {
    uint64_t product;
    if (refTicks == 0 || __builtin_mul_overflow(ticks, refHz, &product)) {
        return STATUS_ERR_INVALID;
    }
    *out = product / refTicks;
    return STATUS_OK;
}

uint64_t timeMedian3(uint64_t a, uint64_t b, uint64_t c) {
    if (a > b) {
        uint64_t t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        b = c;
    }
    return a > b ? a : b;
}

uint32_t timeRefDelta(uint32_t now, uint32_t then, uint32_t mask) {
    return (now - then) & mask;
}

/* --- calendar / RTC ------------------------------------------------------------------------ */

static bool isLeap(uint32_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

uint32_t timeDaysInMonth(uint32_t year, uint32_t mon) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon < 1 || mon > 12) {
        return 0;
    }
    return days[mon - 1] + ((mon == 2 && isLeap(year)) ? 1u : 0u);
}

/* Howard Hinnant's days_from_civil, in unsigned arithmetic (valid for year >= 1970). */
uint64_t timeCivilToEpoch(uint32_t year, uint32_t mon, uint32_t day, uint32_t hour, uint32_t min,
                          uint32_t sec) {
    uint64_t y = year - (mon <= 2 ? 1u : 0u);
    uint64_t era = y / 400;
    uint64_t yoe = y - era * 400;
    uint64_t doy = (153 * (uint64_t)(mon > 2 ? mon - 3 : mon + 9) + 2) / 5 + day - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint64_t days = era * 146097 + doe - 719468;
    return days * 86400 + (uint64_t)hour * 3600 + (uint64_t)min * 60 + sec;
}

/* One register: BCD (each nibble 0-9) or binary; false on a bad BCD nibble. */
static bool rtcField(uint8_t v, bool binary, uint32_t *out) {
    if (binary) {
        *out = v;
        return true;
    }
    if ((v & 0x0F) > 9 || (v >> 4) > 9) {
        return false;
    }
    *out = (uint32_t)(v >> 4) * 10 + (v & 0x0F);
    return true;
}

Status timeRtcDecode(const TimeRtcRaw *raw, uint64_t *epoch, TimeCivil *civil) {
    bool binary = (raw->regB & 0x04) != 0;
    bool h24 = (raw->regB & 0x02) != 0;
    uint32_t sec, min, hour, day, mon, yy, cent = 0;
    bool pm = !h24 && (raw->hour & 0x80) != 0;
    uint8_t hourReg = h24 ? raw->hour : (uint8_t)(raw->hour & 0x7F);
    if (!rtcField(raw->sec, binary, &sec) || !rtcField(raw->min, binary, &min) ||
        !rtcField(hourReg, binary, &hour) || !rtcField(raw->day, binary, &day) ||
        !rtcField(raw->mon, binary, &mon) || !rtcField(raw->year, binary, &yy)) {
        return STATUS_ERR_INVALID;
    }
    if (raw->hasCentury && !rtcField(raw->century, binary, &cent)) {
        return STATUS_ERR_INVALID;
    }
    if (!h24) {
        if (hour < 1 || hour > 12) {
            return STATUS_ERR_INVALID;
        }
        hour = (hour % 12) + (pm ? 12u : 0u);
    }
    if (yy > 99) {
        return STATUS_ERR_INVALID;
    }
    uint32_t year = (raw->hasCentury && cent >= 19 && cent <= 29) ? cent * 100 + yy : 2000 + yy;
    if (year < 1970 || sec > 59 || min > 59 || hour > 23 || mon < 1 || mon > 12 || day < 1 ||
        day > timeDaysInMonth(year, mon)) {
        return STATUS_ERR_INVALID;
    }
    *epoch = timeCivilToEpoch(year, mon, day, hour, min, sec);
    if (civil != NULL) {
        *civil = (TimeCivil){year, mon, day, hour, min, sec};
    }
    return STATUS_OK;
}

/* --- timer heap ---------------------------------------------------------------------------- */

static bool timerBefore(const TimerObj *a, const TimerObj *b) {
    return a->deadlineNs < b->deadlineNs || (a->deadlineNs == b->deadlineNs && a->seq < b->seq);
}

static void heapPlace(TimerHeap *h, uint32_t i, TimerObj *t) {
    h->slot[i] = t;
    t->heapIndex = i;
}

static void siftUp(TimerHeap *h, uint32_t i) {
    TimerObj *t = h->slot[i];
    while (i > 0) {
        uint32_t parent = (i - 1) / 2;
        if (!timerBefore(t, h->slot[parent])) {
            break;
        }
        heapPlace(h, i, h->slot[parent]);
        i = parent;
    }
    heapPlace(h, i, t);
}

static void siftDown(TimerHeap *h, uint32_t i) {
    TimerObj *t = h->slot[i];
    for (;;) {
        uint32_t child = 2 * i + 1;
        if (child >= h->count) {
            break;
        }
        if (child + 1 < h->count && timerBefore(h->slot[child + 1], h->slot[child])) {
            child++;
        }
        if (!timerBefore(h->slot[child], t)) {
            break;
        }
        heapPlace(h, i, h->slot[child]);
        i = child;
    }
    heapPlace(h, i, t);
}

void timerHeapInit(TimerHeap *h) {
    h->count = 0;
    h->nextSeq = 0;
    for (uint32_t i = 0; i < TIMER_HEAP_CAP; i++) {
        h->slot[i] = NULL;
    }
}

Status timerHeapInsert(TimerHeap *h, TimerObj *t) {
    if (h->count >= TIMER_HEAP_CAP) {
        return STATUS_ERR_NO_MEMORY;
    }
    t->seq = h->nextSeq++;
    t->state = TIMER_ARMED;
    uint32_t i = h->count++;
    heapPlace(h, i, t);
    siftUp(h, i);
    return STATUS_OK;
}

void timerHeapRemove(TimerHeap *h, TimerObj *t) {
    uint32_t i = t->heapIndex;
    uint32_t last = --h->count;
    TimerObj *moved = h->slot[last];
    h->slot[last] = NULL;
    if (i == last) {
        return;
    }
    heapPlace(h, i, moved);
    if (i > 0 && timerBefore(moved, h->slot[(i - 1) / 2])) {
        siftUp(h, i);
    } else {
        siftDown(h, i);
    }
}

TimerObj *timerHeapPeek(const TimerHeap *h) {
    return h->count == 0 ? NULL : h->slot[0];
}

TimerObj *timerHeapPop(TimerHeap *h) {
    TimerObj *t = timerHeapPeek(h);
    if (t != NULL) {
        timerHeapRemove(h, t);
    }
    return t;
}

bool timerHeapCheck(const TimerHeap *h) {
    if (h->count > TIMER_HEAP_CAP) {
        return false;
    }
    for (uint32_t i = 0; i < h->count; i++) {
        TimerObj *t = h->slot[i];
        if (t == NULL || t->heapIndex != i) {
            return false;
        }
        if (i > 0 && timerBefore(t, h->slot[(i - 1) / 2])) {
            return false;
        }
    }
    for (uint32_t i = h->count; i < TIMER_HEAP_CAP; i++) {
        if (h->slot[i] != NULL) {
            return false;
        }
    }
    return true;
}
