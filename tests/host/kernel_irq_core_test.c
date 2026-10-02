/* Host tests for kernel/arch/x86_64/irq-core.c (M3.2, D-172/D-174): the vector bitmap allocator,
 * IOAPIC redirection-entry encoding, ISA IRQ -> GSI resolution (against QEMU's real MADTs and
 * synthetic override lists) and GSI -> IOAPIC lookup. */
#include "acpi-tables.h"
#include "acpi_fixture.h"
#include "framework/test.h"
#include "irq-core.h"

#include <stdlib.h>
#include <string.h>

TEST(irqVectorMapInitMarksReservedRanges) {
    IrqVectorMap m;
    irqVectorMapInit(&m);
    ASSERT_EQ(irqVectorMapAllocatedCount(&m), 0u);
    ASSERT_TRUE(!irqVectorMapIsAllocated(&m, 47));
    ASSERT_TRUE(!irqVectorMapIsAllocated(&m, 48));
    ASSERT_TRUE(!irqVectorMapIsAllocated(&m, 240));
    ASSERT_TRUE(!irqVectorMapIsAllocated(&m, 255));
    ASSERT_TRUE(!irqVectorMapIsAllocated(&m, 1000));
}

TEST(irqVectorMapAllocLowestFirstAndExhausts) {
    IrqVectorMap m;
    irqVectorMapInit(&m);
    uint32_t v;
    ASSERT_EQ(irqVectorMapAlloc(&m, &v), STATUS_OK);
    ASSERT_EQ(v, 48u);
    ASSERT_EQ(irqVectorMapAlloc(&m, &v), STATUS_OK);
    ASSERT_EQ(v, 49u);
    ASSERT_TRUE(irqVectorMapIsAllocated(&m, 48));
    ASSERT_EQ(irqVectorMapFree(&m, 48), STATUS_OK);
    ASSERT_EQ(irqVectorMapAlloc(&m, &v), STATUS_OK); /* the hole is reused first */
    ASSERT_EQ(v, 48u);
    uint32_t last = 0;
    uint32_t n = 2;
    while (irqVectorMapAlloc(&m, &v) == STATUS_OK) {
        ASSERT_TRUE(v >= 48 && v <= 239);
        last = v;
        n++;
        ASSERT_TRUE(n <= 192);
    }
    ASSERT_EQ(n, 192u);
    ASSERT_EQ(last, 239u);
    ASSERT_EQ(irqVectorMapAllocatedCount(&m), 192u);
    ASSERT_EQ(irqVectorMapAlloc(&m, &v), STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(irqVectorMapFree(&m, 239), STATUS_OK);
    ASSERT_EQ(irqVectorMapAlloc(&m, &v), STATUS_OK);
    ASSERT_EQ(v, 239u);
}

TEST(irqVectorMapFreeRejectsMisuse) {
    IrqVectorMap m;
    irqVectorMapInit(&m);
    uint32_t v;
    ASSERT_EQ(irqVectorMapAlloc(&m, &v), STATUS_OK);
    ASSERT_EQ(irqVectorMapFree(&m, v), STATUS_OK);
    ASSERT_EQ(irqVectorMapFree(&m, v), STATUS_ERR_INVALID); /* double free */
    ASSERT_EQ(irqVectorMapFree(&m, 47), STATUS_ERR_INVALID);
    ASSERT_EQ(irqVectorMapFree(&m, 240), STATUS_ERR_INVALID);
    ASSERT_EQ(irqVectorMapFree(&m, 0xFF), STATUS_ERR_INVALID);
    ASSERT_EQ(irqVectorMapFree(&m, 0x1000), STATUS_ERR_INVALID);
    ASSERT_EQ(irqVectorMapFree(&m, 100), STATUS_ERR_INVALID); /* never allocated */
    ASSERT_EQ(irqVectorMapAllocatedCount(&m), 0u);
}

TEST(irqCoreRteEncodeBitLayout) {
    ASSERT_EQ(irqCoreRteEncode(0x30, false, false, false, 0), (uint64_t)0x30);
    ASSERT_EQ(irqCoreRteEncode(0x30, true, false, false, 0), (uint64_t)0x2030);
    ASSERT_EQ(irqCoreRteEncode(0x30, false, true, false, 0), (uint64_t)0x8030);
    ASSERT_EQ(irqCoreRteEncode(0x30, false, false, true, 0), (uint64_t)0x10030);
    ASSERT_EQ(irqCoreRteEncode(0xEF, true, true, true, 0xFE), (uint64_t)0xFE0000000001A0EFull);
    ASSERT_EQ(irqCoreRteEncode(0x31, false, false, false, 5) >> 56, (uint64_t)5);
}

TEST(irqCoreIoapicPinsFromVersion) {
    ASSERT_EQ(irqCoreIoapicPins(0x00170020), 24u); /* QEMU: max redirection entry 23 */
    ASSERT_EQ(irqCoreIoapicPins(0x00000011), 1u);
    ASSERT_EQ(irqCoreIoapicPins(0x00FF0011), 256u);
}

TEST(irqCoreGsiLookupAndOverlap) {
    IrqGsiRange r[2] = {{0, 24}, {24, 8}};
    uint32_t pin;
    ASSERT_EQ(irqCoreGsiLookup(r, 2, 0, &pin), 0);
    ASSERT_EQ(pin, 0u);
    ASSERT_EQ(irqCoreGsiLookup(r, 2, 23, &pin), 0);
    ASSERT_EQ(pin, 23u);
    ASSERT_EQ(irqCoreGsiLookup(r, 2, 24, &pin), 1);
    ASSERT_EQ(pin, 0u);
    ASSERT_EQ(irqCoreGsiLookup(r, 2, 31, &pin), 1);
    ASSERT_EQ(pin, 7u);
    ASSERT_EQ(irqCoreGsiLookup(r, 2, 32, &pin), -1);
    ASSERT_EQ(irqCoreGsiLookup(r, 0, 0, &pin), -1);
    ASSERT_TRUE(!irqCoreGsiOverlaps(r, 2, (IrqGsiRange){32, 24}));
    ASSERT_TRUE(irqCoreGsiOverlaps(r, 2, (IrqGsiRange){20, 8}));
    ASSERT_TRUE(irqCoreGsiOverlaps(r, 2, (IrqGsiRange){31, 1}));
    ASSERT_TRUE(irqCoreGsiOverlaps(r, 2, (IrqGsiRange){0, 1}));
    ASSERT_TRUE(irqCoreGsiOverlaps(r, 2, (IrqGsiRange){5, 0})); /* zero pins is never valid */
    ASSERT_TRUE(!irqCoreGsiOverlaps(r, 0, (IrqGsiRange){0, 24}));
}

TEST(irqCoreGsiRangesNeverOverflow) {
    IrqGsiRange hi = {UINT32_MAX - 3, 256}; /* base + pins wraps in 32 bits */
    uint32_t pin;
    ASSERT_EQ(irqCoreGsiLookup(&hi, 1, UINT32_MAX, &pin), 0);
    ASSERT_EQ(pin, 3u);
    ASSERT_EQ(irqCoreGsiLookup(&hi, 1, 5, &pin), -1);
    ASSERT_TRUE(irqCoreGsiOverlaps(&hi, 1, (IrqGsiRange){UINT32_MAX, 1}));
    ASSERT_TRUE(!irqCoreGsiOverlaps(&hi, 1, (IrqGsiRange){0, 24}));
}

static AcpiIso iso(uint8_t bus, uint8_t source, uint32_t gsi, uint16_t flags) {
    AcpiIso i = {bus, source, gsi, flags};
    return i;
}

TEST(irqCoreIsaRouteDefaults) {
    IrqIsaRoute r;
    bool warn = true;
    ASSERT_EQ(irqCoreIsaRoute(NULL, 0, 1, &r, &warn), STATUS_OK);
    ASSERT_EQ(r.gsi, 1u);
    ASSERT_TRUE(!r.activeLow);
    ASSERT_TRUE(!r.level);
    ASSERT_TRUE(!warn);
    ASSERT_EQ(irqCoreIsaRoute(NULL, 0, 15, &r, NULL), STATUS_OK);
    ASSERT_EQ(r.gsi, 15u);
    ASSERT_EQ(irqCoreIsaRoute(NULL, 0, 16, &r, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(irqCoreIsaRoute(NULL, 0, 1, NULL, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(irqCoreIsaRoute(NULL, 3, 1, &r, NULL), STATUS_ERR_INVALID);
}

TEST(irqCoreIsaRouteOverrides) {
    AcpiIso isos[] = {iso(0, 0, 2, 0), iso(0, 5, 5, 0xD), iso(0, 9, 9, 0xF), iso(0, 11, 11, 0x5)};
    IrqIsaRoute r;
    bool warn = false;
    ASSERT_EQ(irqCoreIsaRoute(isos, 4, 0, &r, &warn), STATUS_OK);
    ASSERT_EQ(r.gsi, 2u);
    ASSERT_TRUE(!r.activeLow && !r.level);
    ASSERT_EQ(irqCoreIsaRoute(isos, 4, 5, &r, &warn), STATUS_OK);
    ASSERT_TRUE(!r.activeLow && r.level); /* polarity 01 = high, trigger 11 = level */
    ASSERT_EQ(irqCoreIsaRoute(isos, 4, 9, &r, &warn), STATUS_OK);
    ASSERT_TRUE(r.activeLow && r.level);
    ASSERT_EQ(irqCoreIsaRoute(isos, 4, 11, &r, &warn), STATUS_OK);
    ASSERT_TRUE(!r.activeLow && !r.level); /* polarity high, trigger edge */
    ASSERT_TRUE(!warn);
    ASSERT_EQ(irqCoreIsaRoute(isos, 4, 8, &r, &warn), STATUS_OK); /* untouched: identity */
    ASSERT_EQ(r.gsi, 8u);
    ASSERT_EQ(irqCoreIsaRoute(isos, 4, 2, &r, &warn), STATUS_ERR_NOT_FOUND); /* shadowed by 0->2 */
}

TEST(irqCoreIsaRouteReservedFlagsConformAndWarn) {
    AcpiIso isos[] = {iso(0, 3, 3, 0x2), iso(0, 4, 4, 0x8)};
    IrqIsaRoute r;
    bool warn = false;
    ASSERT_EQ(irqCoreIsaRoute(isos, 2, 3, &r, &warn), STATUS_OK);
    ASSERT_TRUE(!r.activeLow && !r.level);
    ASSERT_TRUE(warn);
    warn = false;
    ASSERT_EQ(irqCoreIsaRoute(isos, 2, 4, &r, &warn), STATUS_OK);
    ASSERT_TRUE(!r.activeLow && !r.level);
    ASSERT_TRUE(warn);
}

TEST(irqCoreIsaRouteIgnoresBadOverrides) {
    AcpiIso isos[] = {
        iso(1, 3, 20, 0xF),  /* not the ISA bus */
        iso(0, 16, 21, 0xF), /* source out of range */
        iso(0, 4, 4, 0xD),   /* first for source 4 wins */
        iso(0, 4, 22, 0xF),  /* later duplicate source */
        iso(0, 6, 4, 0xF),   /* later override of an already claimed GSI */
    };
    IrqIsaRoute r;
    bool warn = false;
    ASSERT_EQ(irqCoreIsaRoute(isos, 5, 3, &r, &warn), STATUS_OK);
    ASSERT_EQ(r.gsi, 3u);
    ASSERT_TRUE(!r.level);
    ASSERT_TRUE(warn);
    ASSERT_EQ(irqCoreIsaRoute(isos, 5, 4, &r, NULL), STATUS_OK);
    ASSERT_EQ(r.gsi, 4u);
    ASSERT_TRUE(r.level && !r.activeLow);
    ASSERT_EQ(irqCoreIsaRoute(isos, 5, 6, &r, NULL), STATUS_OK); /* its override was ignored */
    ASSERT_EQ(r.gsi, 6u);
    ASSERT_TRUE(!r.level);
}

static void qemuFw(const char *dir) {
    AcpiFx fx;
    if (acpiFxLoad(&fx, dir) != 0) {
        fprintf(stderr, "  cannot load fixture %s (run from the repo root)\n", dir);
        hostTestFailures++;
        return;
    }
    AcpiPhysOps ops = acpiFxOps(&fx);
    AcpiTableSet set;
    ASSERT_EQ(acpiTablesLoad(&ops, fx.rsdpPhys, &set), STATUS_OK);
    AcpiInfo *info = calloc(1, sizeof(*info));
    ASSERT_TRUE(info != NULL);
    acpiParseAll(&set, info);
    ASSERT_EQ(info->madtStatus, STATUS_OK);
    const AcpiMadtInfo *m = &info->madt;
    IrqIsaRoute r;
    bool warn = false;
    ASSERT_EQ(irqCoreIsaRoute(m->isos, m->isoCount, 0, &r, &warn), STATUS_OK);
    ASSERT_EQ(r.gsi, 2u);
    ASSERT_TRUE(!r.activeLow && !r.level);
    ASSERT_EQ(irqCoreIsaRoute(m->isos, m->isoCount, 9, &r, &warn), STATUS_OK);
    ASSERT_EQ(r.gsi, 9u);
    ASSERT_TRUE(!r.activeLow && r.level);
    ASSERT_EQ(irqCoreIsaRoute(m->isos, m->isoCount, 8, &r, &warn), STATUS_OK);
    ASSERT_EQ(r.gsi, 8u);
    ASSERT_TRUE(!r.activeLow && !r.level);
    ASSERT_EQ(irqCoreIsaRoute(m->isos, m->isoCount, 2, &r, &warn), STATUS_ERR_NOT_FOUND);
    ASSERT_TRUE(!warn);
    IrqGsiRange range = {m->ioapics[0].gsiBase, 24};
    ASSERT_EQ(irqCoreGsiLookup(&range, 1, 2, NULL), 0);
    free(info);
    acpiTablesFree(&ops, &set);
    acpiFxRelease(&fx);
}

TEST(irqCoreIsaRouteQemuUefiTables) {
    qemuFw("tests/data/acpi/qemu-q35/uefi");
}

TEST(irqCoreIsaRouteQemuBiosTables) {
    qemuFw("tests/data/acpi/qemu-q35/bios");
}
