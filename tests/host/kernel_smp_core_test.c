/* Host tests for kernel/core/smp-core.c and cmdline's value parsing (M3.5, D-193). */
#include "acpi-tables.h"
#include "bootinfo.h"
#include "cmdline.h"
#include "framework/test.h"
#include "smp.h"
#include "time-core.h"

#include <string.h>

TEST(cmdlineFindValueTakesTheLastToken) {
    char v[16];
    ASSERT_TRUE(cmdlineFindValue("a=1 cpus=2 b cpus=4", "cpus", v, sizeof(v)));
    ASSERT_TRUE(strcmp(v, "4") == 0);
    ASSERT_TRUE(!cmdlineFindValue("xcpus=2 cpus", "cpus", v, sizeof(v)));
    ASSERT_TRUE(cmdlineFindValue("cpus=", "cpus", v, sizeof(v)));
    ASSERT_TRUE(strcmp(v, "") == 0);
    ASSERT_TRUE(cmdlineFindValue("ktest=all", "ktest", v, sizeof(v)));
    ASSERT_TRUE(strcmp(v, "all") == 0);
}

TEST(cmdlineParseUintAcceptsOnlyDecimal) {
    uint32_t n = 77;
    ASSERT_TRUE(cmdlineParseUint("0", &n) && n == 0);
    ASSERT_TRUE(cmdlineParseUint("4294967295", &n) && n == 4294967295u);
    n = 5;
    ASSERT_TRUE(!cmdlineParseUint("4294967296", &n) && n == 5);
    ASSERT_TRUE(!cmdlineParseUint("", &n));
    ASSERT_TRUE(!cmdlineParseUint("-1", &n));
    ASSERT_TRUE(!cmdlineParseUint("4x", &n));
    ASSERT_TRUE(!cmdlineParseUint(" 4", &n));
}

TEST(smpParseCpusOption) {
    bool present, invalid;
    ASSERT_EQ(smpParseCpusOption("a b", 64, &present, &invalid), 64u);
    ASSERT_TRUE(!present && !invalid);
    ASSERT_EQ(smpParseCpusOption("cpus=3", 64, &present, &invalid), 3u);
    ASSERT_TRUE(present && !invalid);
    ASSERT_EQ(smpParseCpusOption("cpus=1000", 64, &present, &invalid), 64u);
    ASSERT_TRUE(present && !invalid);
    ASSERT_EQ(smpParseCpusOption("cpus=0", 64, &present, &invalid), 64u);
    ASSERT_TRUE(present && invalid);
    ASSERT_EQ(smpParseCpusOption("cpus=four", 64, &present, &invalid), 64u);
    ASSERT_TRUE(present && invalid);
}

static AcpiCpu cpu(uint32_t apicId, uint32_t flags) {
    AcpiCpu c;
    memset(&c, 0, sizeof(c));
    c.apicId = apicId;
    c.uid = apicId;
    c.flags = flags;
    return c;
}

TEST(smpSelectApsSkipsBspDisabledAndHighIds) {
    AcpiCpu cpus[6] = {cpu(0, 1), cpu(1, 1), cpu(2, 2), cpu(300, 1), cpu(3, 1), cpu(255, 1)};
    uint32_t out[8];
    uint32_t skipped;
    /* xAPIC BSP: 2 is online-capable only, 300 and 255 are unaddressable. */
    uint32_t n = smpSelectAps(cpus, 6, 0, false, 64, out, 8, &skipped);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(out[0], 1u);
    ASSERT_EQ(out[1], 3u);
    ASSERT_EQ(skipped, 2u);
    /* x2APIC BSP: the large ids are fine. */
    n = smpSelectAps(cpus, 6, 0, true, 64, out, 8, &skipped);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(out[1], 300u);
    ASSERT_EQ(skipped, 0u);
    /* cpus=2 means one AP; cpus=1 none. */
    ASSERT_EQ(smpSelectAps(cpus, 6, 0, true, 2, out, 8, &skipped), 1u);
    ASSERT_EQ(smpSelectAps(cpus, 6, 0, true, 1, out, 8, &skipped), 0u);
    /* The BSP need not be the first entry. */
    ASSERT_EQ(smpSelectAps(cpus, 6, 3, true, 64, out, 8, &skipped), 4u);
    ASSERT_EQ(out[0], 0u);
}

static BootMemRegion region(uint64_t base, uint64_t len, uint32_t type) {
    BootMemRegion r;
    memset(&r, 0, sizeof(r));
    r.base = base;
    r.length = len;
    r.type = type;
    return r;
}

TEST(smpPickTrampolinePageChoosesHighestUsablePageBelowTheLimit) {
    uint64_t p = 0;
    BootMemRegion m1[] = {region(0, 0x9FC00, BOOT_MEM_USABLE),
                          region(0x100000, 0x1000000, BOOT_MEM_USABLE)};
    ASSERT_TRUE(smpPickTrampolinePage(m1, 2, &p));
    ASSERT_EQ(p, 0x9E000u); /* ends at the 0x9F000 limit */
    BootMemRegion m2[] = {region(0x1000, 0x6000, BOOT_MEM_USABLE),
                          region(0x7000, 0x1000, BOOT_MEM_RESERVED)};
    ASSERT_TRUE(smpPickTrampolinePage(m2, 2, &p));
    ASSERT_EQ(p, 0x6000u);
    /* An unaligned region shrinks to whole pages. */
    BootMemRegion m3[] = {region(0x1800, 0x2000, BOOT_MEM_USABLE)};
    ASSERT_TRUE(smpPickTrampolinePage(m3, 1, &p));
    ASSERT_EQ(p, 0x2000u);
    /* Nothing: page 0 is never used, a region under one page is skipped, only >= 1 MiB memory. */
    BootMemRegion m4[] = {region(0, 0x1000, BOOT_MEM_USABLE),
                          region(0x2800, 0x800, BOOT_MEM_USABLE),
                          region(0x100000, 0x100000, BOOT_MEM_USABLE)};
    ASSERT_TRUE(!smpPickTrampolinePage(m4, 3, &p));
}

TEST(timeTscPingpongBestPicksTheSmallestRoundTrip) {
    /* The responder's TSC is 1000 ticks ahead. Round 1 has the tightest trip (rtt 20): its estimate
     * is exact; the others are skewed by asymmetric delays. */
    uint64_t t0[3] = {100, 200, 300};
    uint64_t t1[3] = {1190, 1210, 1380};
    uint64_t t2[3] = {200, 220, 500};
    int64_t off;
    uint64_t rtt;
    ASSERT_EQ(timeTscPingpongBest(t0, t1, t2, 3, &off, &rtt), STATUS_OK);
    ASSERT_EQ(rtt, 20u);
    ASSERT_EQ(off, 1000);
    /* A responder that is behind gives a negative offset. */
    uint64_t u1[1] = {5000};
    uint64_t u0[1] = {10000}, u2[1] = {10100};
    ASSERT_EQ(timeTscPingpongBest(u0, u1, u2, 1, &off, &rtt), STATUS_OK);
    ASSERT_EQ(off, 5000 - 10050);
    /* Unusable input. */
    uint64_t bad0[1] = {10}, bad2[1] = {5};
    ASSERT_EQ(timeTscPingpongBest(bad0, u1, bad2, 1, &off, &rtt), STATUS_ERR_INVALID);
    ASSERT_EQ(timeTscPingpongBest(t0, t1, t2, 0, &off, &rtt), STATUS_ERR_INVALID);
}
