/* Host tests for kernel/core/time-core.c (M3.3, D-176..D-180): scaling and calibration arithmetic,
 * epoch conversion and the CMOS RTC decoder. The epoch vectors below were checked with
 * `date -u -d ... +%s`. */
#include "framework/test.h"
#include "time-core.h"

#include <stdint.h>
#include <stdlib.h>

/* Independent reference for floor(v * mult / 2^32): a 32-bit split multiply. */
static uint64_t refScale(uint64_t v, uint64_t mult) {
    uint64_t vLo = v & 0xFFFFFFFFu, vHi = v >> 32;
    uint64_t mLo = mult & 0xFFFFFFFFu, mHi = mult >> 32;
    uint64_t ll = vLo * mLo, lh = vLo * mHi, hl = vHi * mLo, hh = vHi * mHi;
    /* (v*mult) >> 32 = hh<<32 + lh + hl + (ll>>32), wrapped to 64 bits */
    return (hh << 32) + lh + hl + (ll >> 32);
}

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return rngState;
}

TEST(timeScaleMatchesSplitMultiply) {
    for (int i = 0; i < 100000; i++) {
        uint64_t v = rnd() >> (rnd() % 64);
        uint64_t m = rnd() >> (rnd() % 64);
        ASSERT_EQ(timeScale(v, m), refScale(v, m));
    }
    ASSERT_EQ(timeScale(0, 12345), 0u);
    ASSERT_EQ(timeScale(UINT64_MAX, 1), 0xFFFFFFFFu);
    ASSERT_EQ(timeScale(UINT64_MAX, (uint64_t)1 << 32), UINT64_MAX);
    ASSERT_EQ(timeScale(1000, (uint64_t)1 << 32), 1000u);
}

TEST(timeMakeMultRoundTrips) {
    const uint64_t rates[] = {3579545ull,    14318180ull,   100000000ull,  1000000000ull,
                              4500000000ull, 4294967295ull, 4294967296ull, 4294967297ull};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        uint64_t hz = rates[i], mult;
        ASSERT_EQ(timeMakeMult(hz, 1000000000ull, &mult), STATUS_OK);
        uint64_t ns = timeScale(hz, mult);
        uint64_t slack = (hz >> 32) + 1; /* mult is floored: at most ceil(hz / 2^32) ns short */
        ASSERT_TRUE(ns <= 1000000000ull);
        ASSERT_TRUE(ns >= 1000000000ull - slack);
        /* the other direction: ns -> ticks */
        uint64_t back;
        ASSERT_EQ(timeMakeMult(1000000000ull, hz, &back), STATUS_OK);
        uint64_t ticks = timeScale(1000000000ull, back);
        ASSERT_TRUE(ticks <= hz && ticks + 2 >= hz);
    }
    /* 100 ms of TSC at 4.5 GHz */
    uint64_t m;
    ASSERT_EQ(timeMakeMult(1000000000ull, 4500000000ull, &m), STATUS_OK);
    uint64_t t = timeScale(100000000ull, m);
    ASSERT_TRUE(t <= 450000000ull && t + 2 >= 450000000ull);
}

TEST(timeMakeMultRejectsBadInput) {
    uint64_t m = 77;
    ASSERT_EQ(timeMakeMult(0, 1000, &m), STATUS_ERR_INVALID);
    ASSERT_EQ(timeMakeMult(1, (uint64_t)1 << 32, &m), STATUS_ERR_INVALID); /* to/from >= 2^32 */
    ASSERT_EQ(timeMakeMult((uint64_t)1 << 40, ((uint64_t)1 << 40) + ((uint64_t)1 << 33), &m),
              STATUS_ERR_INVALID); /* remainder >= 2^32 */
    ASSERT_EQ(m, 77u);
    ASSERT_EQ(timeMakeMult(1, 4294967295ull, &m), STATUS_OK);
}

TEST(timeCalcHzBasicsAndOverflow) {
    uint64_t hz;
    /* 50 ms of a 3579545 Hz PM timer = 178977 ticks; TSC counted 225,000,000 */
    ASSERT_EQ(timeCalcHz(225000000ull, 178977, 3579545, &hz), STATUS_OK);
    ASSERT_TRUE(hz > 4499000000ull && hz < 4501000000ull);
    ASSERT_EQ(timeCalcHz(5, 0, 1000, &hz), STATUS_ERR_INVALID);
    ASSERT_EQ(timeCalcHz(UINT64_MAX, 1, 2, &hz), STATUS_ERR_INVALID);
    ASSERT_EQ(timeCalcHz(0, 5, 1000, &hz), STATUS_OK);
    ASSERT_EQ(hz, 0u);
}

TEST(timeMedian3AllOrders) {
    uint64_t v[3] = {10, 20, 30};
    int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(timeMedian3(v[perm[i][0]], v[perm[i][1]], v[perm[i][2]]), 20u);
    }
    ASSERT_EQ(timeMedian3(5, 5, 9), 5u);
    ASSERT_EQ(timeMedian3(9, 5, 5), 5u);
    ASSERT_EQ(timeMedian3(7, 7, 7), 7u);
}

TEST(timeRefDeltaWraps) {
    ASSERT_EQ(timeRefDelta(100, 40, 0xFFFFFF), 60u);
    ASSERT_EQ(timeRefDelta(10, 0xFFFFF0, 0xFFFFFF), 26u);       /* 24-bit wrap */
    ASSERT_EQ(timeRefDelta(10, 0xFFFFFFF0u, 0xFFFFFFFFu), 26u); /* 32-bit wrap */
    ASSERT_EQ(timeRefDelta(5, 5, 0xFFFFFF), 0u);
    /* a PM timer exposing garbage above bit 23 is masked off */
    ASSERT_EQ(timeRefDelta(0xAB000010u, 0x00000005u, 0xFFFFFF), 11u);
}

TEST(timeCivilToEpochVectors) {
    ASSERT_EQ(timeCivilToEpoch(1970, 1, 1, 0, 0, 0), 0u);
    ASSERT_EQ(timeCivilToEpoch(2000, 1, 1, 0, 0, 0), 946684800u);
    ASSERT_EQ(timeCivilToEpoch(2000, 2, 29, 0, 0, 0), 951782400u);
    ASSERT_EQ(timeCivilToEpoch(2024, 2, 29, 12, 0, 0), 1709208000u);
    ASSERT_EQ(timeCivilToEpoch(2038, 1, 19, 3, 14, 8), 2147483648u);
    ASSERT_EQ(timeCivilToEpoch(2099, 12, 31, 23, 59, 59), 4102444799u);
    ASSERT_EQ(timeCivilToEpoch(2100, 3, 1, 0, 0, 0), 4107542400u);
}

TEST(timeCivilToEpochEveryDayAdvances) {
    uint64_t expect = 0;
    for (uint32_t y = 1970; y < 2400; y++) {
        for (uint32_t m = 1; m <= 12; m++) {
            uint32_t dim = timeDaysInMonth(y, m);
            ASSERT_TRUE(dim >= 28 && dim <= 31);
            for (uint32_t d = 1; d <= dim; d++) {
                ASSERT_EQ(timeCivilToEpoch(y, m, d, 0, 0, 0), expect);
                expect += 86400;
            }
        }
    }
    ASSERT_EQ(timeDaysInMonth(2000, 2), 29u);
    ASSERT_EQ(timeDaysInMonth(1900, 2), 28u);
    ASSERT_EQ(timeDaysInMonth(2024, 2), 29u);
    ASSERT_EQ(timeDaysInMonth(2023, 2), 28u);
    ASSERT_EQ(timeDaysInMonth(2023, 0), 0u);
    ASSERT_EQ(timeDaysInMonth(2023, 13), 0u);
}

/* BCD, 24h, with a century register: 2026-10-02 13:45:59 */
static TimeRtcRaw bcdRaw(void) {
    TimeRtcRaw r = {.sec = 0x59,
                    .min = 0x45,
                    .hour = 0x13,
                    .day = 0x02,
                    .mon = 0x10,
                    .year = 0x26,
                    .century = 0x20,
                    .regB = 0x02,
                    .hasCentury = true};
    return r;
}

TEST(timeRtcDecodeBcd24h) {
    TimeRtcRaw r = bcdRaw();
    uint64_t e = 0;
    TimeCivil c = {0};
    ASSERT_EQ(timeRtcDecode(&r, &e, &c), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 13, 45, 59));
    ASSERT_EQ(c.year, 2026u);
    ASSERT_EQ(c.mon, 10u);
    ASSERT_EQ(c.day, 2u);
    ASSERT_EQ(c.hour, 13u);
    ASSERT_EQ(c.min, 45u);
    ASSERT_EQ(c.sec, 59u);
}

TEST(timeRtcDecodeBinary24h) {
    TimeRtcRaw r = {.sec = 59,
                    .min = 45,
                    .hour = 13,
                    .day = 2,
                    .mon = 10,
                    .year = 26,
                    .century = 20,
                    .regB = 0x06,
                    .hasCentury = true};
    uint64_t e = 0;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 13, 45, 59));
}

TEST(timeRtcDecode12hMode) {
    uint64_t e = 0;
    TimeRtcRaw r = bcdRaw();
    r.regB = 0x00; /* BCD, 12-hour */
    r.hour = 0x12; /* 12 AM = 00:xx */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 0, 45, 59));
    r.hour = 0x12 | 0x80; /* 12 PM = 12:xx */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 12, 45, 59));
    r.hour = 0x01 | 0x80; /* 1 PM = 13:xx */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 13, 45, 59));
    r.hour = 0x01; /* 1 AM */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 1, 45, 59));
    r.hour = 0x00; /* hour 0 does not exist in 12h mode */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r.hour = 0x13;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    /* binary 12h */
    TimeRtcRaw b = {.sec = 1,
                    .min = 2,
                    .hour = 11 | 0x80,
                    .day = 3,
                    .mon = 4,
                    .year = 5,
                    .regB = 0x04,
                    .hasCentury = false};
    ASSERT_EQ(timeRtcDecode(&b, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2005, 4, 3, 23, 2, 1));
}

TEST(timeRtcDecodeRejectsGarbage) {
    uint64_t e = 12345;
    TimeRtcRaw r = bcdRaw();
    r.sec = 0x6A; /* bad nibble */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.sec = 0x60;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.mon = 0x13;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.mon = 0x00;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.mon = 0x02;
    r.day = 0x30; /* Feb 30 */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.day = 0x00;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.hour = 0x24;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.min = 0x60;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.century = 0x1A; /* bad century nibble */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r = bcdRaw();
    r.regB = 0x06; /* binary: 0x59 = 89 s */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(e, 12345u); /* never written on failure */
}

TEST(timeRtcDecodeCenturyRules) {
    uint64_t e;
    TimeRtcRaw r = bcdRaw();
    r.hasCentury = false; /* no register: 20yy */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 13, 45, 59));
    r = bcdRaw();
    r.century = 0x00; /* implausible century: fall back to 20yy */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, 13, 45, 59));
    r = bcdRaw();
    r.century = 0x19; /* 1926 < 1970 */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r.year = 0x85; /* 1985 */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, timeCivilToEpoch(1985, 10, 2, 13, 45, 59));
    r = bcdRaw();
    r.century = 0x21; /* 2100-02-29: 2100 is not a leap year */
    r.year = 0x00;
    r.mon = 0x02;
    r.day = 0x29;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r.century = 0x20; /* 2000-02-29 is */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_OK);
    ASSERT_EQ(e, 951782400u + (13 * 3600 + 45 * 60 + 59));
}

/* --- bug-sweeper (M3.3 finish) --------------------------------------------------------------- */

/* timeScale(x, timeMakeMult(from, to)) against the exact floor(x * to / from) over random rates
 * (1 Hz .. 2^40 Hz both ways; the documented refusals checked too) and values: the floored
 * multiplier may lose at most x / 2^32 + 1. */
TEST(timeMakeMultScaleMatchesExactRatio) {
    for (int i = 0; i < 200000; i++) {
        uint64_t from = (rnd() >> (24 + rnd() % 40)) + 1;
        uint64_t to = (rnd() >> (24 + rnd() % 40)) + 1;
        uint64_t mult;
        Status st = timeMakeMult(from, to, &mult);
        if (to / from >= ((uint64_t)1 << 32) || to % from >= ((uint64_t)1 << 32)) {
            ASSERT_EQ(st, STATUS_ERR_INVALID); /* the documented limits */
            continue;
        }
        ASSERT_EQ(st, STATUS_OK);
        uint64_t x = rnd() >> (rnd() % 64);
        unsigned __int128 exact = (unsigned __int128)x * to / from;
        if (exact > UINT64_MAX) {
            continue; /* the result does not fit; timeScale truncates by contract */
        }
        uint64_t got = timeScale(x, mult);
        ASSERT_TRUE(got <= (uint64_t)exact);
        ASSERT_TRUE((uint64_t)exact - got <= (x >> 32) + 1);
    }
}

TEST(timeCalcHzOverflowEdge) {
    uint64_t hz = 0;
    /* (2^32 + 1) * (2^32 - 1) = 2^64 - 1 exactly: representable */
    ASSERT_EQ(timeCalcHz(4294967297ull, 1, 4294967295ull, &hz), STATUS_OK);
    ASSERT_EQ(hz, UINT64_MAX);
    hz = 5;
    ASSERT_EQ(timeCalcHz(4294967297ull, 1, 4294967296ull, &hz), STATUS_ERR_INVALID);
    ASSERT_EQ(hz, 5u); /* untouched on error */
}

/* Every hour of the day through the 12-hour encodings (BCD and binary) decodes to itself. */
TEST(timeRtcDecode12hEveryHourBothEncodings) {
    for (uint32_t h = 0; h < 24; h++) {
        uint32_t h12 = h % 12 == 0 ? 12 : h % 12;
        uint8_t pm = h >= 12 ? 0x80 : 0;
        uint64_t e;
        TimeCivil c;
        TimeRtcRaw r = bcdRaw();
        r.regB = 0x00;
        r.hour = (uint8_t)(pm | ((h12 / 10) << 4) | (h12 % 10));
        ASSERT_EQ(timeRtcDecode(&r, &e, &c), STATUS_OK);
        ASSERT_EQ(c.hour, h);
        ASSERT_EQ(e, timeCivilToEpoch(2026, 10, 2, h, 45, 59));
        TimeRtcRaw b = {.sec = 59,
                        .min = 45,
                        .hour = (uint8_t)(pm | h12),
                        .day = 2,
                        .mon = 10,
                        .year = 26,
                        .century = 20,
                        .regB = 0x04,
                        .hasCentury = true};
        ASSERT_EQ(timeRtcDecode(&b, &e, &c), STATUS_OK);
        ASSERT_EQ(c.hour, h);
    }
    TimeRtcRaw r = bcdRaw();
    uint64_t e = 7;
    r.regB = 0x00;
    r.hour = 0x80; /* "PM 0" */
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r.hour = 0x80 | 0x13;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    r.regB = 0x02; /* 24h: bit 7 is not a PM flag, so 0x92 is a bad BCD hour */
    r.hour = 0x92;
    ASSERT_EQ(timeRtcDecode(&r, &e, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(e, 7u);
}
