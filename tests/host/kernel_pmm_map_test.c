/* Host tests for kernel/mm/pmm-map.c's pure BootInfo-map scan (D-079/D-080, ROADMAP M2.2). */
#include "framework/test.h"
#include "pmm-internal.h"

static BootMemRegion region(uint64_t base, uint64_t length, BootMemType type) {
    return (BootMemRegion){.base = base, .length = length, .type = (uint32_t)type, .reserved = 0};
}

/* Invariant (a) from the M2.2 design: every USABLE page is accounted for exactly once, across
 * lowReservedPages/usable ranges/unmappedPages. */
static uint64_t usablePagesTotal(const PmmMap *m) {
    uint64_t total = m->lowReservedPages + m->unmappedPages;
    for (uint32_t i = 0; i < m->usableCount; i++) {
        total += m->usable[i].endPfn - m->usable[i].startPfn;
    }
    return total;
}

TEST(pmmMapScanQemuLike) {
    /* A typical q35/512 MiB boot: low USABLE RAM, the kernel/loader-reclaim block, some
     * reserved/ACPI space, a framebuffer BAR. */
    BootMemRegion regions[] = {
        region(0, 0x100000, BOOT_MEM_RESERVED),              /* [0, 1 MiB) */
        region(0x100000, 0x700000, BOOT_MEM_USABLE),         /* [1 MiB, 8 MiB) */
        region(0x800000, 0x200000, BOOT_MEM_KERNEL),         /* [8 MiB, 10 MiB) */
        region(0xA00000, 0x100000, BOOT_MEM_LOADER_RECLAIM), /* [10 MiB, 11 MiB) */
        region(0xB00000, 0x1F500000, BOOT_MEM_USABLE),       /* [11 MiB, 512 MiB) */
        region(0xFEC00000, 0x1000, BOOT_MEM_RESERVED),       /* IOAPIC MMIO hole */
        region(0xFD000000, 0x1000000, BOOT_MEM_FRAMEBUFFER), /* GPU BAR */
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 7, &map), STATUS_OK);

    ASSERT_EQ(map.lowReservedPages, 0u); /* nothing USABLE below 1 MiB in this layout */
    ASSERT_TRUE(map.usableCount >= 1);
    ASSERT_EQ(map.unmappedPages, 0u);
    ASSERT_EQ(usablePagesTotal(&map), (0x700000ull + 0x1F500000ull) / 4096);
    ASSERT_EQ(map.typePages[BOOT_MEM_KERNEL], 0x200000u / 4096);
    ASSERT_EQ(map.loaderReclaimCount, 1u);
    ASSERT_EQ(map.loaderReclaim[0].physBase, 0xA00000ull);
    ASSERT_EQ(map.loaderReclaim[0].length, 0x100000ull);

    /* Every managed byte must fall inside some span. */
    ASSERT_TRUE(map.spanCount >= 1);
    for (uint32_t i = 0; i < map.spanCount; i++) {
        ASSERT_EQ(map.spans[i].startPfn % (1ull << PMM_MAX_ORDER), 0u);
        ASSERT_EQ(map.spans[i].endPfn % (1ull << PMM_MAX_ORDER), 0u);
    }
    ASSERT_EQ(map.maxPfn, map.spans[map.spanCount - 1].endPfn);
}

TEST(pmmMapScanRegionStraddling4Gib) {
    BootMemRegion regions[] = {
        region(0x100000, 0x0FFF00000ull, BOOT_MEM_USABLE),      /* [1 MiB, 4 GiB) */
        region(0x100000000ull, 0x40000000ull, BOOT_MEM_USABLE), /* [4 GiB, 5 GiB) */
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 2, &map), STATUS_OK);
    ASSERT_EQ(usablePagesTotal(&map), (0x0FFF00000ull + 0x40000000ull) / 4096);
    /* A span may (and here does) cover both zones -- zone membership is decided per-pfn at alloc
     * time (pmmZoneOfPfn), not by the span table. */
    ASSERT_TRUE(map.maxPfn > (0x100000000ull >> 12));
}

TEST(pmmMapScanUsableBelow1Mib) {
    BootMemRegion regions[] = {
        region(0, 0x80000, BOOT_MEM_USABLE),        /* [0, 512 KiB): all withheld */
        region(0x80000, 0x180000, BOOT_MEM_USABLE), /* [512 KiB, 1.5 MiB): straddles 1 MiB */
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 2, &map), STATUS_OK);
    ASSERT_EQ(map.lowReservedPages, 0x100000ull / 4096); /* everything below 1 MiB */
    ASSERT_EQ(map.usableCount, 1u);
    ASSERT_EQ(map.usable[0].startPfn, 0x100000ull >> 12);
    ASSERT_EQ(map.usable[0].endPfn, 0x200000ull >> 12);
    ASSERT_EQ(usablePagesTotal(&map), 0x200000ull / 4096);
}

TEST(pmmMapScanBeyondHhdmWindow) {
    uint64_t base = BOOTINFO_HHDM_SIZE - 0x100000ull; /* last 1 MiB inside the window */
    BootMemRegion regions[] = {
        region(base, 0x300000ull, BOOT_MEM_USABLE), /* straddles the 64 TiB HHDM limit */
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 1, &map), STATUS_OK);
    ASSERT_EQ(map.unmappedPages, 0x200000ull / 4096); /* the 2 MiB beyond the window */
    ASSERT_EQ(map.usableCount, 1u);
    ASSERT_EQ(map.usable[0].endPfn, BOOTINFO_HHDM_SIZE >> 12);
    ASSERT_EQ(usablePagesTotal(&map), 0x300000ull / 4096);
}

TEST(pmmMapScanAdjacentManagedTypesMergeIntoOneSpan) {
    BootMemRegion regions[] = {
        region(0x100000, 0x100000, BOOT_MEM_USABLE),         /* [1, 2 MiB) */
        region(0x200000, 0x100000, BOOT_MEM_KERNEL),         /* [2, 3 MiB) */
        region(0x300000, 0x100000, BOOT_MEM_LOADER_RECLAIM), /* [3, 4 MiB) */
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 3, &map), STATUS_OK);
    ASSERT_EQ(map.spanCount, 1u); /* all three widen/merge into a single order-10-aligned span */
}

TEST(pmmMapScanSmallGapStillMergesEnvelopes) {
    /* A RESERVED hole under 4 MiB (1024 frames) between two managed regions: both regions' order-
     * 10-aligned envelopes overlap even though the raw regions don't touch, so they merge into one
     * span (padding stays PAGE_STATE_RESERVED, per pmm.h). */
    BootMemRegion regions[] = {
        region(0x100000, 0x1000, BOOT_MEM_USABLE),   /* [1 MiB, 1 MiB+4 KiB) */
        region(0x101000, 0x1000, BOOT_MEM_RESERVED), /* a tiny hole */
        region(0x102000, 0x1000, BOOT_MEM_KERNEL),   /* [1 MiB+8 KiB, ...) */
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 3, &map), STATUS_OK);
    ASSERT_EQ(map.spanCount, 1u);
}

TEST(pmmMapScanReservedAndFramebufferOnlyProduceNoSpan) {
    BootMemRegion regions[] = {
        region(0, 0x100000, BOOT_MEM_RESERVED),
        region(0x100000, 0x100000, BOOT_MEM_ACPI_NVS),
        region(0x200000, 0x100000, BOOT_MEM_BAD),
        region(0x300000, 0x100000, BOOT_MEM_FRAMEBUFFER),
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 4, &map), STATUS_OK);
    ASSERT_EQ(map.spanCount, 0u);
    ASSERT_EQ(map.usableCount, 0u);
    ASSERT_EQ(map.maxPfn, 0u);
}

TEST(pmmMapScanSpanCapOverflowFails) {
    /* PMM_MAX_SPANS regions, each separated by a gap wide enough (well past order-10 alignment)
     * that none of them merge -- the (PMM_MAX_SPANS+1)th must fail rather than overflow. */
    BootMemRegion regions[PMM_MAX_SPANS + 1];
    uint64_t stride = 0x10000000ull; /* 256 MiB: far more than the 4 MiB merge distance */
    for (int i = 0; i < PMM_MAX_SPANS + 1; i++) {
        regions[i] = region((uint64_t)i * stride, 0x100000ull, BOOT_MEM_KERNEL);
    }
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, PMM_MAX_SPANS + 1, &map), STATUS_ERR_INVALID);
}

TEST(pmmMapScanMisalignedRegionFails) {
    BootMemRegion regions[] = {region(0x100000, 0x1234, BOOT_MEM_USABLE)};
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 1, &map), STATUS_ERR_INVALID);
}

TEST(pmmMapScanUnknownTypeFails) {
    BootMemRegion regions[] = {region(0x100000, 0x1000, 99)};
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 1, &map), STATUS_ERR_INVALID);
}

TEST(pmmMapScanRecordsAcpiReclaim) {
    BootMemRegion regions[] = {
        region(0x100000, 0x100000, BOOT_MEM_USABLE),
        region(0x200000, 0x8000, BOOT_MEM_ACPI_RECLAIM),
        region(0x208000, 0x1000, BOOT_MEM_ACPI_NVS),
        region(0x300000, 0x4000, BOOT_MEM_ACPI_RECLAIM),
    };
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, 4, &map), STATUS_OK);
    ASSERT_EQ(map.acpiReclaimCount, 2u);
    ASSERT_EQ(map.acpiReclaim[0].physBase, 0x200000ull);
    ASSERT_EQ(map.acpiReclaim[0].length, 0x8000ull);
    ASSERT_EQ(map.acpiReclaim[1].physBase, 0x300000ull);
    ASSERT_EQ(map.acpiReclaimDroppedPages, 0ull);
    ASSERT_EQ(map.typePages[BOOT_MEM_ACPI_RECLAIM], 12ull);
    ASSERT_EQ(map.loaderReclaimCount, 0u); /* the two reclaim lists are independent */
}

TEST(pmmMapScanAcpiReclaimOverflowIsCountedNotFatal) {
    /* PMM_MAX_RECLAIM_RANGES + 3 one-page ACPI_RECLAIM regions (firmware controls the count):
     * the extras are counted, never an error, and never lose the usable/span bookkeeping. */
    enum { N = PMM_MAX_RECLAIM_RANGES + 3 };
    BootMemRegion regions[N + 1];
    regions[0] = region(0x100000, 0x100000, BOOT_MEM_USABLE);
    for (int i = 0; i < N; i++) {
        regions[1 + i] = region(0x200000 + (uint64_t)i * 0x2000, 0x1000, BOOT_MEM_ACPI_RECLAIM);
    }
    PmmMap map;
    ASSERT_EQ(pmmMapScan(regions, N + 1, &map), STATUS_OK);
    ASSERT_EQ(map.acpiReclaimCount, (uint32_t)PMM_MAX_RECLAIM_RANGES);
    ASSERT_EQ(map.acpiReclaimDroppedPages, 3ull);
    ASSERT_EQ(map.typePages[BOOT_MEM_ACPI_RECLAIM], (uint64_t)N);
    ASSERT_EQ(map.usableCount, 1u);
}

/* pmmReclaimClip: what pmmReclaimLoaderMemory()/pmmReclaimAcpiMemory() may free of one recorded
 * range -- never below 1 MiB (D-080), never at or past the HHDM window. Straddling either edge
 * keeps exactly the inside part; a range entirely outside yields nothing. */
TEST(pmmReclaimClipEdges) {
    const uint64_t mib = 0x100000, hhdm = BOOTINFO_HHDM_SIZE;
    struct {
        uint64_t base, length;
        bool ok;
        uint64_t wantBase, wantEnd;
    } cases[] = {
        {0x200000, 0x8000, true, 0x200000, 0x208000},       /* plain */
        {0x9F000, 0x1000, false, 0, 0},                     /* entirely below 1 MiB */
        {0x0, mib, false, 0, 0},                            /* ends exactly at 1 MiB */
        {0xFF000, 0x2000, true, mib, mib + 0x1000},         /* straddles 1 MiB */
        {0x0, 2 * mib, true, mib, 2 * mib},                 /* from 0 across 1 MiB */
        {mib, 0x1000, true, mib, mib + 0x1000},             /* starts exactly at 1 MiB */
        {hhdm - 0x1000, 0x1000, true, hhdm - 0x1000, hhdm}, /* last page inside */
        {hhdm - 0x1000, 0x3000, true, hhdm - 0x1000, hhdm}, /* straddles the HHDM limit */
        {hhdm, 0x1000, false, 0, 0},                        /* starts exactly at the limit */
        {hhdm + 0x5000, 0x10000, false, 0, 0},              /* entirely past it */
        {0x0, hhdm + 0x1000, true, mib, hhdm},              /* both edges at once */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PmmReclaimRange r = {.physBase = cases[i].base, .length = cases[i].length};
        uint64_t b = 0xDEAD, e = 0xBEEF;
        bool ok = pmmReclaimClip(&r, &b, &e);
        ASSERT_EQ(ok, cases[i].ok);
        if (ok) {
            ASSERT_EQ(b, cases[i].wantBase);
            ASSERT_EQ(e, cases[i].wantEnd);
        }
    }
}
