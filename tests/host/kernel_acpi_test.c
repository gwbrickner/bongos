/* Host tests for kernel/drivers/acpi/acpi-tables.c (M3.1, D-166/D-167): the loader and the
 * FADT/MADT/MCFG/HPET/IVRS parsers against synthetic tables built here (the stored QEMU tables get
 * their own tests in kernel_acpi_stored_test.c). Every loader test runs on a fake physical memory
 * whose allocator counts live allocations, and ends by asserting that count is back to zero. */
#include "acpi-tables.h"
#include "framework/test.h"

#include <stdlib.h>
#include <string.h>

/* ---- fake physical memory ---------------------------------------------------------------- */

#define FAKE_MAX 160

typedef struct {
    uint64_t phys;
    uint32_t len;
    uint8_t *bytes;
} FakeBlock;

typedef struct {
    FakeBlock blocks[FAKE_MAX];
    int count;
    int live;          /* outstanding allocations */
    int allocs;        /* total alloc calls */
    int failAllocAt;   /* 1-based alloc call that returns NULL; 0 = never */
    uint64_t flipPhys; /* if nonzero: the 2nd read of a table at this phys has Length bumped */
    int readsOfFlip;
} FakeMem;

static Status fakeRead(void *ctx, uint64_t phys, void *dst, uint32_t len) {
    FakeMem *m = ctx;
    for (int i = 0; i < m->count; i++) {
        FakeBlock *b = &m->blocks[i];
        if (phys >= b->phys && phys - b->phys <= b->len && len <= b->len - (phys - b->phys)) {
            memcpy(dst, b->bytes + (phys - b->phys), len);
            if (m->flipPhys == phys && len > 36) {
                ((uint8_t *)dst)[4]++; /* the second (full) read disagrees with the header read */
            }
            return STATUS_OK;
        }
    }
    return STATUS_ERR_INVALID;
}

static void *fakeAlloc(void *ctx, uint32_t len) {
    FakeMem *m = ctx;
    m->allocs++;
    if (m->failAllocAt != 0 && m->allocs == m->failAllocAt) {
        return NULL;
    }
    m->live++;
    return malloc(len);
}

static void fakeFree(void *ctx, void *p, uint32_t len) {
    FakeMem *m = ctx;
    (void)len;
    m->live--;
    free(p);
}

static AcpiPhysOps fakeOps(FakeMem *m) {
    AcpiPhysOps o = {m, fakeRead, fakeAlloc, fakeFree};
    return o;
}

static void fakePut(FakeMem *m, uint64_t phys, const uint8_t *bytes, uint32_t len) {
    FakeBlock *b = &m->blocks[m->count++];
    b->phys = phys;
    b->len = len;
    b->bytes = malloc(len);
    memcpy(b->bytes, bytes, len);
}

static void fakeRelease(FakeMem *m) {
    for (int i = 0; i < m->count; i++) {
        free(m->blocks[i].bytes);
    }
    m->count = 0;
}

/* ---- table builders ---------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}
static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Header + zero body of `len` bytes, with a valid checksum. */
static void mkTable(uint8_t *t, const char *sig, uint32_t len, uint8_t rev) {
    memset(t, 0, len);
    memcpy(t, sig, 4);
    put32(t + 4, len);
    t[8] = rev;
    memcpy(t + 10, "BONGOS", 6);
    t[9] = (uint8_t)(0 - acpiChecksum(t, len));
}

static void fixSum(uint8_t *t, uint32_t len) {
    t[9] = 0;
    t[9] = (uint8_t)(0 - acpiChecksum(t, len));
}

static void mkRsdp(uint8_t *r, uint8_t rev, uint32_t rsdt, uint64_t xsdt) {
    memset(r, 0, 36);
    memcpy(r, "RSD PTR ", 8);
    memcpy(r + 9, "BONGOS", 6);
    r[15] = rev;
    put32(r + 16, rsdt);
    r[8] = 0;
    r[8] = (uint8_t)(0 - acpiChecksum(r, 20));
    if (rev >= 2) {
        put32(r + 20, 36);
        put64(r + 24, xsdt);
        r[32] = 0;
        r[32] = (uint8_t)(0 - acpiChecksum(r, 36));
    }
}

#define RSDP_PHYS 0xE0000u
#define RSDT_PHYS 0x100000u
#define XSDT_PHYS 0x101000u

/* A root table with `n` entries (8 bytes each for XSDT, 4 for RSDT). */
static uint8_t *mkRoot(const char *sig, bool x, const uint64_t *phys, int n, int extra) {
    uint32_t es = x ? 8 : 4;
    uint32_t len = 36 + (uint32_t)n * es + (uint32_t)extra;
    uint8_t *t = calloc(1, len);
    mkTable(t, sig, len, 1);
    for (int i = 0; i < n; i++) {
        if (x) {
            put64(t + 36 + i * 8, phys[i]);
        } else {
            put32(t + 36 + i * 4, (uint32_t)phys[i]);
        }
    }
    fixSum(t, len);
    return t;
}

/* A memory with an RSDP rev `rev`, an RSDT listing `rsdtPhys[]` and (rev>=2) an XSDT listing the
 * same. Returns nothing; tables themselves are added by the caller with fakePut. */
static void mkRoots(FakeMem *m, uint8_t rev, const uint64_t *list, int n) {
    uint8_t rsdp[36];
    mkRsdp(rsdp, rev, RSDT_PHYS, rev >= 2 ? XSDT_PHYS : 0);
    fakePut(m, RSDP_PHYS, rsdp, rev >= 2 ? 36 : 20);
    uint8_t *r = mkRoot("RSDT", false, list, n, 0);
    fakePut(m, RSDT_PHYS, r, acpiRd32(r + 4));
    free(r);
    if (rev >= 2) {
        uint8_t *x = mkRoot("XSDT", true, list, n, 0);
        fakePut(m, XSDT_PHYS, x, acpiRd32(x + 4));
        free(x);
    }
}

static void addSimple(FakeMem *m, uint64_t phys, const char *sig, uint32_t len) {
    uint8_t *t = malloc(len);
    mkTable(t, sig, len, 1);
    fakePut(m, phys, t, len);
    free(t);
}

/* ---- loader tests ------------------------------------------------------------------------ */

TEST(acpiLoadRsdtOnly) {
    FakeMem m = {0};
    uint64_t list[] = {0x200000, 0x201000};
    mkRoots(&m, 0, list, 2);
    addSimple(&m, 0x200000, "FOO1", 40);
    addSimple(&m, 0x201000, "FOO2", 64);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_TRUE(!s->usedXsdt);
    ASSERT_EQ(s->count, 3u);
    ASSERT_EQ(s->rejected, 0u);
    ASSERT_EQ(s->rsdpLength, 20u);
    ASSERT_TRUE(acpiTablesFind(s, "FOO2", 0) != NULL);
    ASSERT_EQ(acpiTablesFind(s, "FOO2", 0)->length, 64u);
    ASSERT_TRUE(acpiTablesFind(s, "FOO2", 1) == NULL);
    ASSERT_EQ(s->dsdtIndex, -1);
    ASSERT_TRUE((s->warnings & ACPI_WARN_NO_DSDT) != 0);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadXsdtPreferred) {
    FakeMem m = {0};
    uint64_t list[] = {0x200000};
    mkRoots(&m, 2, list, 1);
    addSimple(&m, 0x200000, "FOO1", 40);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_TRUE(s->usedXsdt);
    ASSERT_EQ(s->rsdpLength, 36u);
    ASSERT_TRUE(s->tables[0].signature[0] == 'X');
    ASSERT_EQ(s->warnings & ACPI_WARN_XSDT_FALLBACK, 0u);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadRsdpErrors) {
    FakeMem m = {0};
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, 0, s), STATUS_ERR_NOT_FOUND);

    uint8_t rsdp[36];
    mkRsdp(rsdp, 0, RSDT_PHYS, 0);
    rsdp[0] = 'X'; /* bad signature */
    fakePut(&m, RSDP_PHYS, rsdp, 20);
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_ERR_INVALID);
    ASSERT_EQ(s->count, 0u);
    fakeRelease(&m);

    mkRsdp(rsdp, 0, RSDT_PHYS, 0);
    rsdp[10] ^= 0x55; /* bad v1 checksum */
    fakePut(&m, RSDP_PHYS, rsdp, 20);
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_ERR_INVALID);
    fakeRelease(&m);
    ASSERT_EQ(m.live, 0);
    free(s);
}

TEST(acpiLoadRsdpV2Fallbacks) {
    uint64_t list[] = {0x200000};
    for (int variant = 0; variant < 2; variant++) {
        FakeMem m = {0};
        mkRoots(&m, 2, list, 1);
        addSimple(&m, 0x200000, "FOO1", 40);
        if (variant == 0) { /* bad extended checksum */
            m.blocks[0].bytes[33] ^= 0x10;
        } else { /* xsdt == 0 */
            uint8_t rsdp[36];
            mkRsdp(rsdp, 2, RSDT_PHYS, 0);
            memcpy(m.blocks[0].bytes, rsdp, 36);
        }
        AcpiPhysOps ops = fakeOps(&m);
        AcpiTableSet *s = calloc(1, sizeof(*s));
        ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
        ASSERT_TRUE(!s->usedXsdt);
        ASSERT_EQ(s->count, 2u);
        ASSERT_EQ((s->warnings & ACPI_WARN_RSDP_V2_BAD) != 0, variant == 0);
        ASSERT_EQ(s->rsdpLength, variant == 0 ? 20u : 36u);
        acpiTablesFree(&ops, s);
        ASSERT_EQ(m.live, 0);
        free(s);
        fakeRelease(&m);
    }
}

TEST(acpiLoadBadXsdtFallsBackToRsdt) {
    FakeMem m = {0};
    uint64_t list[] = {0x200000};
    mkRoots(&m, 2, list, 1);
    addSimple(&m, 0x200000, "FOO1", 40);
    m.blocks[2].bytes[36] ^= 0x01; /* XSDT body: breaks its checksum */
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_TRUE(!s->usedXsdt);
    ASSERT_TRUE((s->warnings & ACPI_WARN_XSDT_FALLBACK) != 0);
    ASSERT_TRUE(s->tables[0].signature[0] == 'R');
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadBadXsdtNoRsdtIsInvalid) {
    FakeMem m = {0};
    uint8_t rsdp[36];
    mkRsdp(rsdp, 2, 0, XSDT_PHYS);
    fakePut(&m, RSDP_PHYS, rsdp, 36);
    uint64_t list[] = {0x200000};
    uint8_t *x = mkRoot("XSDT", true, list, 1, 0);
    x[36] ^= 1;
    fakePut(&m, XSDT_PHYS, x, acpiRd32(x + 4));
    free(x);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_ERR_INVALID);
    ASSERT_EQ(s->count, 0u);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadHostileLengths) {
    uint64_t list[] = {0x200000, 0x201000, 0xFFFFFFFFFFFFFFF0ull};
    FakeMem m = {0};
    uint8_t rsdp[36];
    mkRsdp(rsdp, 2, 0, XSDT_PHYS);
    fakePut(&m, RSDP_PHYS, rsdp, 36);
    uint8_t *x = mkRoot("XSDT", true, list, 3, 0);
    fakePut(&m, XSDT_PHYS, x, acpiRd32(x + 4));
    free(x);
    uint8_t t[64];
    mkTable(t, "SHRT", 64, 1);
    put32(t + 4, 20); /* Length < header */
    fakePut(&m, 0x200000, t, 64);
    mkTable(t, "HUGE", 64, 1);
    put32(t + 4, ACPI_TABLE_MAX_LEN + 1);
    fakePut(&m, 0x201000, t, 64);
    uint8_t ov[40];
    mkTable(ov, "OVFL", 40, 1);
    put32(ov + 4, 0x100);
    fakePut(&m, 0xFFFFFFFFFFFFFFF0ull, ov, 40);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_EQ(s->count, 1u); /* only the root */
    ASSERT_EQ(s->rejected, 3u);
    ASSERT_EQ(s->rejects[0].status, STATUS_ERR_INVALID);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadRootTrailingBytes) {
    FakeMem m = {0};
    uint8_t rsdp[36];
    mkRsdp(rsdp, 2, 0, XSDT_PHYS);
    fakePut(&m, RSDP_PHYS, rsdp, 36);
    uint64_t list[] = {0x200000, 0x201000};
    uint8_t *x = mkRoot("XSDT", true, list, 2, 4);
    fakePut(&m, XSDT_PHYS, x, acpiRd32(x + 4));
    free(x);
    addSimple(&m, 0x200000, "FOO1", 40);
    addSimple(&m, 0x201000, "FOO2", 40);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_EQ(s->count, 3u);
    ASSERT_TRUE((s->warnings & ACPI_WARN_ROOT_TRAILING) != 0);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadRejectsBadChildKeepsSiblings) {
    FakeMem m = {0};
    uint64_t list[] = {0x200000, 0x201000, 0x202000};
    mkRoots(&m, 0, list, 3);
    addSimple(&m, 0x200000, "AAAA", 40);
    addSimple(&m, 0x201000, "BBBB", 40);
    m.blocks[m.count - 1].bytes[20] ^= 0x7f; /* bad checksum */
    /* 0x202000: unreadable (never added) */
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_EQ(s->count, 2u);
    ASSERT_EQ(s->rejected, 2u);
    ASSERT_TRUE(acpiTablesFind(s, "AAAA", 0) != NULL);
    ASSERT_TRUE(acpiTablesFind(s, "BBBB", 0) == NULL);
    ASSERT_TRUE(s->rejects[0].signature[0] == 'B');
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadToctouLengthChange) {
    FakeMem m = {0};
    uint64_t list[] = {0x200000};
    mkRoots(&m, 0, list, 1);
    addSimple(&m, 0x200000, "AAAA", 40);
    m.flipPhys = 0x200000;
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_EQ(s->count, 1u);
    ASSERT_EQ(s->rejected, 1u);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadTooManyTablesDropsAndDedups) {
    FakeMem m = {0};
    uint64_t list[ACPI_MAX_TABLES + 6];
    int n = 0;
    for (int i = 0; i < (int)ACPI_MAX_TABLES + 5; i++) {
        list[n++] = 0x200000 + (uint64_t)i * 0x1000;
        addSimple(&m, list[n - 1], "TBL0", 40);
    }
    list[n++] = list[0]; /* a duplicate */
    mkRoots(&m, 0, list, n);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_EQ(s->count, ACPI_MAX_TABLES - 1);
    ASSERT_EQ(s->dropped, 7u); /* 133 entries; 127 slots minus the root hold 126 */
    ASSERT_TRUE((s->warnings & ACPI_WARN_TABLES_DROPPED) != 0);
    ASSERT_TRUE((s->warnings & ACPI_WARN_DUPLICATE_ENTRY) != 0);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadAllocFailureFreesEverything) {
    for (int failAt = 1; failAt <= 4; failAt++) {
        FakeMem m = {0};
        uint64_t list[] = {0x200000, 0x201000, 0x202000};
        mkRoots(&m, 0, list, 3);
        addSimple(&m, 0x200000, "AAAA", 40);
        addSimple(&m, 0x201000, "BBBB", 40);
        addSimple(&m, 0x202000, "CCCC", 40);
        m.failAllocAt = failAt;
        AcpiPhysOps ops = fakeOps(&m);
        AcpiTableSet *s = calloc(1, sizeof(*s));
        ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_ERR_NO_MEMORY);
        ASSERT_EQ(s->count, 0u);
        ASSERT_EQ(m.live, 0);
        free(s);
        fakeRelease(&m);
    }
}

/* FADT builder: a 244-byte revision-5 table by default. */
static void mkFadt(uint8_t *t, uint32_t len, uint32_t dsdt, uint64_t xDsdt) {
    mkTable(t, "FACP", len, len >= 244 ? 5 : 1);
    put32(t + 40, dsdt);
    put16(t + 46, 9);
    put32(t + 56, 0x600);
    put32(t + 64, 0x604);
    put32(t + 76, 0x608);
    t[88] = 4;
    t[89] = 2;
    t[91] = 4;
    put32(t + 112, (1u << 10) | (1u << 8));
    if (len >= 129) {
        t[116] = 1;
        t[117] = 8;
        put64(t + 120, 0xCF9);
        t[128] = 6;
    }
    if (len >= 148) {
        put64(t + 140, xDsdt);
    }
    fixSum(t, len);
}

TEST(acpiLoadFindsDsdtViaFadt) {
    FakeMem m = {0};
    uint64_t list[] = {0x200000};
    mkRoots(&m, 0, list, 1);
    uint8_t fadt[244];
    mkFadt(fadt, 244, 0x300000, 0);
    fakePut(&m, 0x200000, fadt, 244);
    addSimple(&m, 0x300000, "DSDT", 100);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_EQ(s->fadtIndex, 1);
    ASSERT_EQ(s->dsdtIndex, 2);
    ASSERT_EQ(s->tables[2].phys, (uint64_t)0x300000);
    ASSERT_EQ(s->warnings & ACPI_WARN_NO_DSDT, 0u);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

TEST(acpiLoadDsdtMismatchPrefersXDsdt) {
    FakeMem m = {0};
    uint64_t list[] = {0x200000};
    mkRoots(&m, 0, list, 1);
    uint8_t fadt[244];
    mkFadt(fadt, 244, 0x300000, 0x310000);
    fakePut(&m, 0x200000, fadt, 244);
    addSimple(&m, 0x300000, "DSDT", 100);
    addSimple(&m, 0x310000, "DSDT", 120);
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_TRUE(s->dsdtIndex > 0);
    ASSERT_EQ(s->tables[s->dsdtIndex].length, 120u);
    ASSERT_TRUE((s->warnings & ACPI_WARN_DSDT_MISMATCH) != 0);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

/* ---- FADT -------------------------------------------------------------------------------- */

TEST(acpiFadtLegacyBlocks) {
    uint8_t t[116];
    mkFadt(t, 116, 0x1234, 0);
    AcpiFadtInfo f;
    ASSERT_EQ(acpiParseFadt(t, 116, &f), STATUS_OK);
    ASSERT_EQ(f.sciInt, 9u);
    ASSERT_EQ(f.pm1aEvt.address, (uint64_t)0x600);
    ASSERT_EQ(f.pm1aEvt.spaceId, 1u);
    ASSERT_EQ(f.pm1aEvt.bitWidth, 32u);
    ASSERT_EQ(f.pm1aCnt.address, (uint64_t)0x604);
    ASSERT_EQ(f.pmTmr.address, (uint64_t)0x608);
    ASSERT_TRUE(f.pmTimerPresent);
    ASSERT_TRUE(f.pmTimer32Bit);
    ASSERT_TRUE(!f.resetSupported); /* table too short for RESET_REG */
    ASSERT_EQ(f.dsdtPhys, (uint64_t)0x1234);
    ASSERT_EQ(f.pm1bEvt.address, (uint64_t)0);
}

TEST(acpiFadtExtendedOverridesLegacy) {
    uint8_t t[244];
    mkFadt(t, 244, 0x1234, 0x9999);
    put64(t + 208 + 4, 0x1608); /* X_PM_TMR */
    t[208] = 1;
    t[209] = 32;
    fixSum(t, 244);
    AcpiFadtInfo f;
    ASSERT_EQ(acpiParseFadt(t, 244, &f), STATUS_OK);
    ASSERT_EQ(f.pmTmr.address, (uint64_t)0x1608);
    ASSERT_EQ(f.dsdtPhys, (uint64_t)0x9999);
    ASSERT_TRUE(f.dsdtMismatch);
    ASSERT_TRUE(f.resetSupported);
    ASSERT_EQ(f.resetReg.address, (uint64_t)0xCF9);
    ASSERT_EQ(f.resetValue, 6u);
}

TEST(acpiFadtHardwareReducedHasNoTimer) {
    uint8_t t[244];
    mkFadt(t, 244, 0, 0);
    put32(t + 112, (1u << 20));
    fixSum(t, 244);
    AcpiFadtInfo f;
    ASSERT_EQ(acpiParseFadt(t, 244, &f), STATUS_OK);
    ASSERT_TRUE(f.hwReduced);
    ASSERT_TRUE(!f.pmTimerPresent);
}

TEST(acpiFadtRejectsShortAndMismatched) {
    uint8_t t[244];
    mkFadt(t, 244, 0, 0);
    AcpiFadtInfo f;
    ASSERT_EQ(acpiParseFadt(t, 115, &f), STATUS_ERR_INVALID); /* Length field != len */
    uint8_t s[100];
    mkTable(s, "FACP", 100, 1);
    ASSERT_EQ(acpiParseFadt(s, 100, &f), STATUS_ERR_INVALID);
    mkFadt(t, 244, 0, 0);
    t[0] = 'X';
    ASSERT_EQ(acpiParseFadt(t, 244, &f), STATUS_ERR_INVALID);
}

/* ---- MADT -------------------------------------------------------------------------------- */

/* Appends a MADT entry; `t` has room, `*len` grows. */
static void madtAdd(uint8_t *t, uint32_t *len, const uint8_t *e, uint8_t elen) {
    memcpy(t + *len, e, elen);
    *len += elen;
    put32(t + 4, *len);
    fixSum(t, *len);
}

static void madtLapic(uint8_t *t, uint32_t *len, uint8_t uid, uint8_t id, uint32_t flags) {
    uint8_t e[8] = {0, 8, uid, id};
    put32(e + 4, flags);
    madtAdd(t, len, e, 8);
}

static uint32_t madtStart(uint8_t *t) {
    mkTable(t, "APIC", 44, 3);
    put32(t + 36, 0xFEE00000);
    put32(t + 40, 1);
    fixSum(t, 44);
    return 44;
}

TEST(acpiMadtBasics) {
    static uint8_t t[2048];
    uint32_t len = madtStart(t);
    for (uint8_t i = 0; i < 4; i++) {
        madtLapic(t, &len, i, i, 1);
    }
    uint8_t io[12] = {1, 12, 0};
    put32(io + 4, 0xFEC00000);
    madtAdd(t, &len, io, 12);
    uint8_t iso[10] = {2, 10, 0, 0};
    put32(iso + 4, 2);
    put16(iso + 8, 0);
    madtAdd(t, &len, iso, 10);
    uint8_t iso2[10] = {2, 10, 0, 9};
    put32(iso2 + 4, 9);
    put16(iso2 + 8, 0xD);
    madtAdd(t, &len, iso2, 10);
    uint8_t nmi[6] = {4, 6, 0xFF};
    put16(nmi + 3, 0);
    nmi[5] = 1;
    madtAdd(t, &len, nmi, 6);
    AcpiMadtInfo *m = calloc(1, sizeof(*m));
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_OK);
    ASSERT_EQ(m->cpuCount, 4u);
    ASSERT_EQ(m->cpus[3].apicId, 3u);
    ASSERT_EQ(m->lapicAddress, (uint64_t)0xFEE00000);
    ASSERT_TRUE(m->pcatCompat);
    ASSERT_EQ(m->ioapicCount, 1u);
    ASSERT_EQ(m->ioapics[0].address, 0xFEC00000u);
    ASSERT_EQ(m->isoCount, 2u);
    ASSERT_EQ(m->isos[1].source, 9u);
    ASSERT_EQ(m->isos[1].flags, 0xDu);
    ASSERT_EQ(m->lapicNmiCount, 1u);
    ASSERT_EQ(m->lapicNmis[0].uid, ACPI_LAPIC_NMI_ALL);
    ASSERT_EQ(m->lapicNmis[0].lint, 1u);
    free(m);
}

TEST(acpiMadtMalformedEntries) {
    static uint8_t t[2048];
    AcpiMadtInfo *m = calloc(1, sizeof(*m));
    uint32_t len = madtStart(t);
    uint8_t zero[2] = {0, 0}; /* entry length 0: would loop forever */
    madtAdd(t, &len, zero, 2);
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_ERR_INVALID);
    ASSERT_EQ(m->cpuCount, 0u);

    len = madtStart(t);
    uint8_t over[2] = {0, 40}; /* claims 40 bytes, has 2 */
    madtAdd(t, &len, over, 2);
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_ERR_INVALID);

    len = madtStart(t);
    uint8_t shortLapic[6] = {0, 6, 0, 1};
    madtAdd(t, &len, shortLapic, 6);
    madtLapic(t, &len, 1, 0xFF, 1);
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_OK);
    ASSERT_EQ(m->malformedEntries, 2u);
    ASSERT_EQ(m->cpuCount, 0u);

    len = 20;
    mkTable(t, "APIC", 20, 3);
    ASSERT_EQ(acpiParseMadt(t, 20, m), STATUS_ERR_INVALID);
    free(m);
}

TEST(acpiMadtCpuFlagsDedupAndCap) {
    static uint8_t t[8192];
    AcpiMadtInfo *m = calloc(1, sizeof(*m));
    uint32_t len = madtStart(t);
    madtLapic(t, &len, 0, 0, 0); /* disabled */
    madtLapic(t, &len, 1, 1, 2); /* online-capable only */
    madtLapic(t, &len, 2, 2, 1);
    uint8_t x2[16] = {9, 16};
    put32(x2 + 4, 2); /* duplicates APIC ID 2 */
    put32(x2 + 8, 1);
    put32(x2 + 12, 7);
    madtAdd(t, &len, x2, 16);
    put32(x2 + 4, 0x1000); /* a real x2APIC ID */
    madtAdd(t, &len, x2, 16);
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_OK);
    ASSERT_EQ(m->cpusDisabled, 1u);
    ASSERT_EQ(m->cpuCount, 3u);
    ASSERT_TRUE(m->cpus[2].x2apic);
    ASSERT_EQ(m->cpus[2].apicId, 0x1000u);
    ASSERT_EQ(m->cpus[2].uid, 7u);

    /* 300 distinct CPUs: 256 kept, 44 dropped. */
    len = madtStart(t);
    for (uint32_t i = 0; i < 300; i++) {
        put32(x2 + 4, 0x100 + i);
        madtAdd(t, &len, x2, 16);
    }
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_OK);
    ASSERT_EQ(m->cpuCount, ACPI_MAX_CPUS);
    ASSERT_EQ(m->cpusDropped, 44u);
    free(m);
}

TEST(acpiMadtLapicAddressOverride) {
    static uint8_t t[256];
    AcpiMadtInfo *m = calloc(1, sizeof(*m));
    uint32_t len = madtStart(t);
    uint8_t ov[12] = {5, 12};
    put64(ov + 4, 0x1FEE00000ull);
    madtAdd(t, &len, ov, 12);
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_OK);
    ASSERT_EQ(m->lapicAddress, (uint64_t)0x1FEE00000ull);
    free(m);
}

/* ---- MCFG / HPET / IVRS ------------------------------------------------------------------ */

TEST(acpiMcfgParses) {
    uint8_t t[44 + 16 * 3 + 8];
    uint32_t len = 44 + 16 * 3 + 8;
    mkTable(t, "MCFG", len, 1);
    put64(t + 44, 0xB0000000ull);
    put16(t + 52, 0);
    t[54] = 0;
    t[55] = 255;
    put64(t + 60, 0xC0000000ull); /* start > end: skipped */
    t[70] = 9;
    t[71] = 3;
    put64(t + 76, 0); /* base 0: skipped */
    fixSum(t, len);
    AcpiMcfgInfo m;
    ASSERT_EQ(acpiParseMcfg(t, len, &m), STATUS_OK);
    ASSERT_EQ(m.count, 1u);
    ASSERT_EQ(m.malformed, 2u);
    ASSERT_TRUE(m.trailing);
    ASSERT_EQ(m.segs[0].base, (uint64_t)0xB0000000ull);
    ASSERT_EQ(m.segs[0].endBus, 255u);
    mkTable(t, "MCFG", 40, 1);
    ASSERT_EQ(acpiParseMcfg(t, 40, &m), STATUS_ERR_INVALID);
}

TEST(acpiHpetParses) {
    uint8_t t[56];
    mkTable(t, "HPET", 56, 1);
    put32(t + 36, 0x8086A201u);
    t[40] = 0;
    put64(t + 44, 0xFED00000ull);
    t[52] = 0;
    put16(t + 53, 128);
    fixSum(t, 56);
    AcpiHpetInfo h;
    ASSERT_EQ(acpiParseHpet(t, 56, &h), STATUS_OK);
    ASSERT_EQ(h.base, (uint64_t)0xFED00000ull);
    ASSERT_EQ(h.comparators, 3u);
    ASSERT_TRUE(h.counter64);
    ASSERT_EQ(h.minTick, 128u);
    t[40] = 1; /* I/O space: not valid for the HPET */
    ASSERT_EQ(acpiParseHpet(t, 56, &h), STATUS_ERR_INVALID);
    mkTable(t, "HPET", 52, 1);
    ASSERT_EQ(acpiParseHpet(t, 52, &h), STATUS_ERR_INVALID);
}

static void ivrsBlock(uint8_t *t, uint32_t *len, uint8_t type, uint16_t blen) {
    memset(t + *len, 0, blen);
    t[*len] = type;
    put16(t + *len + 2, blen);
    put16(t + *len + 4, 0x0802);
    put64(t + *len + 8, 0xFEB80000ull);
    *len += blen;
    put32(t + 4, *len);
    fixSum(t, *len);
}

TEST(acpiIvrsParses) {
    static uint8_t t[512];
    mkTable(t, "IVRS", 48, 2);
    put32(t + 36, 0x00203000);
    uint32_t len = 48;
    ivrsBlock(t, &len, 0x10, 28);
    ivrsBlock(t, &len, 0x40, 48);
    ivrsBlock(t, &len, 0x20, 32);
    ivrsBlock(t, &len, 0x11, 30); /* short IVHD (< 40): malformed */
    AcpiIvrsInfo v;
    ASSERT_EQ(acpiParseIvrs(t, len, &v), STATUS_OK);
    ASSERT_EQ(v.ivhdCount, 2u);
    ASSERT_EQ(v.ivmdCount, 1u);
    ASSERT_EQ(v.malformed, 1u);
    ASSERT_EQ(v.ivhd[0].base, (uint64_t)0xFEB80000ull);
    ASSERT_EQ(v.ivhd[0].deviceId, 0x0802u);
    ASSERT_EQ(v.ivInfo, 0x00203000u);
    ASSERT_TRUE(v.raw == t);

    uint32_t l2 = 48;
    mkTable(t, "IVRS", 48, 2);
    ivrsBlock(t, &l2, 0x10, 28);
    put16(t + 48 + 2, 0); /* block length 0 */
    ASSERT_EQ(acpiParseIvrs(t, l2, &v), STATUS_ERR_INVALID);
    mkTable(t, "IVRS", 48, 2);
    l2 = 48;
    ivrsBlock(t, &l2, 0x10, 28);
    put16(t + 48 + 2, 400); /* overruns */
    ASSERT_EQ(acpiParseIvrs(t, l2, &v), STATUS_ERR_INVALID);
    mkTable(t, "IVRS", 40, 2);
    ASSERT_EQ(acpiParseIvrs(t, 40, &v), STATUS_ERR_INVALID);
}

/* ---- physical range policy --------------------------------------------------------------- */

TEST(acpiPhysRangePolicy) {
    BootMemRegion map[] = {
        {0x100000, 0x100000, BOOT_MEM_USABLE, 0},
        {0x200000, 0x100000, BOOT_MEM_ACPI_RECLAIM, 0},
        {0x300000, 0x100000, BOOT_MEM_RESERVED, 0},
        {0x400000, 0x100000, BOOT_MEM_ACPI_NVS, 0},
        {0x500000, 0x100000, BOOT_MEM_KERNEL, 0},
        {0x600000, 0x100000, BOOT_MEM_LOADER_RECLAIM, 0},
        {0x800000, 0x100000, BOOT_MEM_FRAMEBUFFER, 0},
        {0x900000, 0x100000, BOOT_MEM_BAD, 0},
    };
    uint32_t n = sizeof(map) / sizeof(map[0]);
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0xF5000, 0x1000));  /* below 1 MiB, no region */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0xFF000, 0x2000));  /* straddles 1 MiB into USABLE */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0x1FF000, 0x2000)); /* USABLE -> ACPI_RECLAIM */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0x300000, 0x1000));
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0x400000, 0x1000));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x500000, 8));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x600000, 8));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x800000, 8));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x900000, 8));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x700000, 8));    /* hole at >= 1 MiB */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0x3FFFF8, 0x10));  /* RESERVED -> NVS */
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x4FFFF8, 0x10)); /* NVS -> KERNEL */
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x7FFFF8, 0x10)); /* into a hole */
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x300000, 0));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0xFFFFFFFFFFFFFFF0ull, 0x100));
}

/* ---- bug-sweeper adversarial tests (M3.1 step 4 sweep) ----------------------------------- */

/* A table whose header can't even be read has no signature: its reject record must say "????",
 * not repeat the signature of whatever table loadOne() read before it (the root, or a sibling). */
TEST(acpiLoadUnreadableRejectHasNoStaleSig) {
    FakeMem m = {0};
    uint64_t list[] = {0x300000, 0x200000, 0x301000};
    mkRoots(&m, 0, list, 3);
    addSimple(&m, 0x200000, "AAAA", 40);
    /* 0x300000 and 0x301000: unreadable (never added) */
    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_EQ(s->count, 2u);
    ASSERT_EQ(s->rejected, 2u);
    ASSERT_EQ(s->rejects[0].phys, (uint64_t)0x300000);
    ASSERT_TRUE(memcmp(s->rejects[0].signature, "????", 4) == 0); /* not the root's "RSDT" */
    ASSERT_EQ(s->rejects[1].phys, (uint64_t)0x301000);
    ASSERT_TRUE(memcmp(s->rejects[1].signature, "????", 4) == 0); /* not the sibling's "AAAA" */
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

/* acpiPhysRangeAllowed() is pure and documents no "validated map" precondition, so a region whose
 * base+length wraps past 2^64 must make it return false, not loop forever (its end used to clamp
 * the cursor back below 1 MiB, which then jumped forward into the same region again). */
TEST(acpiPhysRangeWrappingRegionTerminates) {
    BootMemRegion wrap[] = {{0x100000, 0ull - 0x100000, BOOT_MEM_RESERVED, 0}};
    ASSERT_TRUE(!acpiPhysRangeAllowed(wrap, 1, 0x200000, 0x1000));
    BootMemRegion wrap2[] = {{0x200000, 0ull - 0x100000, BOOT_MEM_RESERVED, 0}};
    ASSERT_TRUE(!acpiPhysRangeAllowed(wrap2, 1, 0x200000, 0x1000));
}

/* Boundary cases of the range policy: the exact 1 MiB edge, a range ending exactly at a region's
 * end, ending exactly at 2^64 (wraps to 0), and the top of the address space with no region. */
TEST(acpiPhysRangePolicyEdges) {
    BootMemRegion map[] = {
        {0x100000, 0x1000, BOOT_MEM_ACPI_RECLAIM, 0},
        {0x101000, 0x1000, BOOT_MEM_ACPI_NVS, 0},
        {0xFFFFFFFFFFFFE000ull, 0x1000, BOOT_MEM_RESERVED, 0},
    };
    uint32_t n = sizeof(map) / sizeof(map[0]);
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0xFFFFF, 1));        /* last byte below 1 MiB */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0xFFFFF, 2));        /* ... and the first above */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0x100000, 0x2000));  /* two regions, exact end */
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0x100000, 0x2001)); /* one byte into the hole */
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, 0, 0x100000, 1));      /* empty map above 1 MiB */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, 0, 0, 0x100000));       /* all of low memory */
    ASSERT_TRUE(acpiPhysRangeAllowed(map, n, 0xFFFFFFFFFFFFE000ull, 0x1000));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0xFFFFFFFFFFFFE000ull, 0x1001));
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0xFFFFFFFFFFFFF000ull, 0x1000)); /* wraps to 0 */
    ASSERT_TRUE(!acpiPhysRangeAllowed(map, n, 0xFFFFFFFFFFFFFFFFull, 1));
}

static uint64_t fuzzState = 0x9E3779B97F4A7C15ull;
static uint32_t fuzzNext(void) {
    fuzzState ^= fuzzState << 13;
    fuzzState ^= fuzzState >> 7;
    fuzzState ^= fuzzState << 17;
    return (uint32_t)(fuzzState >> 16);
}

/* Every parser, fed random bodies of every length from 0 to 600 behind a header whose signature
 * and Length are right (so the body is actually parsed), must stay inside `len` bytes (ASan: the
 * buffer is exactly `len` bytes on the heap) and return OK or INVALID. Entry-structured bodies
 * (small, plausible entry lengths) reach the MADT/IVRS per-type paths. */
TEST(acpiParsersSurviveRandomBodies) {
    static const char *sigs[] = {"FACP", "APIC", "MCFG", "HPET", "IVRS"};
    AcpiFadtInfo *fadt = malloc(sizeof(*fadt));
    AcpiMadtInfo *madt = malloc(sizeof(*madt));
    AcpiMcfgInfo *mcfg = malloc(sizeof(*mcfg));
    AcpiHpetInfo *hpet = malloc(sizeof(*hpet));
    AcpiIvrsInfo *ivrs = malloc(sizeof(*ivrs));
    for (uint32_t len = 0; len <= 600; len++) {
        for (int iter = 0; iter < 8; iter++) {
            uint8_t *t = malloc(len == 0 ? 1 : len);
            for (uint32_t i = 0; i < len; i++) {
                t[i] = (uint8_t)fuzzNext();
            }
            if (iter & 1) { /* plausible entry lengths: 0..24 */
                for (uint32_t i = 44; i + 1 < len; i += 2) {
                    t[i + 1] = (uint8_t)(fuzzNext() % 25);
                }
            }
            for (int k = 0; k < 5; k++) {
                if (len >= 8) {
                    memcpy(t, sigs[k], 4);
                    put32(t + 4, len);
                }
                Status st[5];
                st[0] = acpiParseFadt(t, len, fadt);
                st[1] = acpiParseMadt(t, len, madt);
                st[2] = acpiParseMcfg(t, len, mcfg);
                st[3] = acpiParseHpet(t, len, hpet);
                st[4] = acpiParseIvrs(t, len, ivrs);
                for (int j = 0; j < 5; j++) {
                    ASSERT_TRUE(st[j] == STATUS_OK || st[j] == STATUS_ERR_INVALID);
                    if (j != k) {
                        ASSERT_EQ(st[j], STATUS_ERR_INVALID); /* wrong signature */
                    }
                }
                if (st[1] == STATUS_OK) {
                    ASSERT_TRUE(madt->cpuCount <= ACPI_MAX_CPUS);
                    ASSERT_TRUE(madt->ioapicCount <= ACPI_MAX_IOAPICS);
                    ASSERT_TRUE(madt->isoCount <= ACPI_MAX_ISOS);
                    ASSERT_TRUE(madt->nmiSourceCount <= ACPI_MAX_NMI_SOURCES);
                    ASSERT_TRUE(madt->lapicNmiCount <= ACPI_MAX_LAPIC_NMIS);
                }
                if (st[4] == STATUS_OK) {
                    ASSERT_TRUE(ivrs->ivhdCount <= ACPI_MAX_IVHD);
                    ASSERT_TRUE(ivrs->raw == t && ivrs->rawLen == len);
                }
            }
            free(t);
        }
    }
    free(fadt);
    free(madt);
    free(mcfg);
    free(hpet);
    free(ivrs);
}

/* The loader over a random fake memory: random RSDP revision and root, random entries that point
 * at random (often garbage, sometimes valid) tables, random allocation failures. Whatever happens,
 * the result is OK/INVALID/NO_MEMORY, every kept table is checksum-valid with a matching header,
 * and freeing the set returns every allocation (and a non-OK result leaves none behind). */
TEST(acpiLoadSurvivesRandomMemory) {
    static const char *sigs[] = {"FACP", "APIC", "DSDT", "SSDT", "XSDT", "RSDT", "MCFG"};
    for (int iter = 0; iter < 400; iter++) {
        FakeMem m = {0};
        uint64_t list[24];
        int n = (int)(fuzzNext() % 24);
        for (int i = 0; i < n; i++) {
            uint32_t r = fuzzNext() % 16;
            list[i] = r < 12 ? 0x200000 + (uint64_t)r * 0x1000 : (uint64_t)fuzzNext() << 8;
        }
        for (int i = 0; i < 12; i++) {
            uint32_t len = 36 + fuzzNext() % 300;
            uint8_t *t = malloc(len);
            if (fuzzNext() % 4 == 0) {
                for (uint32_t j = 0; j < len; j++) {
                    t[j] = (uint8_t)fuzzNext();
                }
            } else {
                const char *sig = sigs[fuzzNext() % 7];
                if (sig[0] == 'F' && len < 148) {
                    len = 148 + fuzzNext() % 140;
                    free(t);
                    t = malloc(len);
                }
                mkTable(t, sig, len, (uint8_t)(fuzzNext() % 7));
                for (uint32_t j = 36; j < len; j++) {
                    t[j] = (uint8_t)fuzzNext();
                }
                if (sig[0] == 'F') { /* DSDT / X_DSDT at one of the random blocks, or garbage */
                    put32(t + 40, 0x200000 + (fuzzNext() % 14) * 0x1000);
                    put64(t + 140, fuzzNext() % 2 ? 0 : 0x200000 + (fuzzNext() % 14) * 0x1000);
                }
                if (fuzzNext() % 8 != 0) {
                    fixSum(t, len);
                }
            }
            if (fuzzNext() % 6 == 0) {
                put32(t + 4, fuzzNext()); /* a hostile Length */
            }
            fakePut(&m, 0x200000 + (uint64_t)i * 0x1000, t, len);
            free(t);
        }
        mkRoots(&m, (uint8_t)(fuzzNext() % 4), list, n);
        m.failAllocAt = fuzzNext() % 3 == 0 ? (int)(1 + fuzzNext() % 10) : 0;
        AcpiPhysOps ops = fakeOps(&m);
        AcpiTableSet *s = calloc(1, sizeof(*s));
        Status st = acpiTablesLoad(&ops, RSDP_PHYS, s);
        ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID || st == STATUS_ERR_NO_MEMORY);
        if (st != STATUS_OK) {
            ASSERT_EQ(s->count, 0u);
            ASSERT_EQ(m.live, 0);
        } else {
            ASSERT_TRUE(s->count >= 1 && s->count <= ACPI_MAX_TABLES);
            for (uint32_t i = 0; i < s->count; i++) {
                const AcpiTable *t = &s->tables[i];
                ASSERT_TRUE(t->length >= ACPI_TABLE_HEADER_LEN);
                ASSERT_EQ(acpiRd32(t->data + 4), t->length);
                ASSERT_TRUE(memcmp(t->data, t->signature, 4) == 0);
                ASSERT_EQ(acpiChecksum(t->data, t->length), 0u);
            }
            ASSERT_TRUE(s->dsdtIndex < 0 ||
                        memcmp(s->tables[s->dsdtIndex].signature, "DSDT", 4) == 0);
            ASSERT_TRUE(s->fadtIndex < 0 ||
                        memcmp(s->tables[s->fadtIndex].signature, "FACP", 4) == 0);
            AcpiInfo *info = malloc(sizeof(*info));
            acpiParseAll(s, info);
            free(info);
            acpiTablesFree(&ops, s);
            ASSERT_EQ(m.live, 0);
        }
        free(s);
        fakeRelease(&m);
    }
}

/* ---- bug-sweeper adversarial tests (M3.1 finish sweep) ----------------------------------- */

/* Every 64-bit address field, with values above 4 GiB (every other test, and both QEMU fixtures,
 * keep tables and bases below 4 GiB, so reading any of these as 32 bits went unnoticed): the RSDP's
 * XsdtAddress, the XSDT's entries, the FADT's X_DSDT, the MCFG base and the HPET base. Also the
 * MCFG/HPET fields the other tests leave at 0. */
TEST(acpiLoadAddressesAbove4Gib) {
    const uint64_t xsdtPhys = 0x123456000ull, fadtPhys = 0x100002000ull;
    const uint64_t dsdtPhys = 0x100003000ull, mcfgPhys = 0x200000000ull;
    const uint64_t hpetPhys = 0x100005000ull;
    FakeMem m = {0};
    uint8_t rsdp[36];
    mkRsdp(rsdp, 2, 0, xsdtPhys);
    fakePut(&m, RSDP_PHYS, rsdp, 36);
    uint64_t list[] = {fadtPhys, mcfgPhys, hpetPhys};
    uint8_t *x = mkRoot("XSDT", true, list, 3, 0);
    fakePut(&m, xsdtPhys, x, acpiRd32(x + 4));
    free(x);
    uint8_t fadt[244];
    mkFadt(fadt, 244, 0, dsdtPhys);
    fakePut(&m, fadtPhys, fadt, 244);
    addSimple(&m, dsdtPhys, "DSDT", 100);
    uint8_t mcfg[60];
    mkTable(mcfg, "MCFG", 60, 1);
    put64(mcfg + 44, 0x4000000000ull);
    put16(mcfg + 52, 3);
    mcfg[54] = 0x10;
    mcfg[55] = 0x7F;
    fixSum(mcfg, 60);
    fakePut(&m, mcfgPhys, mcfg, 60);
    uint8_t hpet[56];
    mkTable(hpet, "HPET", 56, 1);
    put32(hpet + 36, 0x8086A201u);
    put64(hpet + 44, 0x1FED00000ull);
    hpet[52] = 2;
    put16(hpet + 53, 0x80);
    hpet[55] = 1;
    fixSum(hpet, 56);
    fakePut(&m, hpetPhys, hpet, 56);

    AcpiPhysOps ops = fakeOps(&m);
    AcpiTableSet *s = calloc(1, sizeof(*s));
    ASSERT_EQ(acpiTablesLoad(&ops, RSDP_PHYS, s), STATUS_OK);
    ASSERT_TRUE(s->usedXsdt);
    ASSERT_EQ(s->xsdtPhys, xsdtPhys);
    ASSERT_EQ(s->count, 5u); /* XSDT, FACP, MCFG, HPET, DSDT */
    ASSERT_EQ(s->tables[0].phys, xsdtPhys);
    ASSERT_EQ(s->tables[1].phys, fadtPhys);
    ASSERT_EQ(s->tables[2].phys, mcfgPhys);
    ASSERT_EQ(s->tables[3].phys, hpetPhys);
    ASSERT_EQ(s->dsdtIndex, 4);
    ASSERT_EQ(s->tables[4].phys, dsdtPhys);
    ASSERT_EQ(s->rejected, 0u);
    AcpiInfo *info = calloc(1, sizeof(*info));
    acpiParseAll(s, info);
    ASSERT_EQ(info->fadtStatus, STATUS_OK);
    ASSERT_EQ(info->fadt.dsdtPhys, dsdtPhys);
    ASSERT_EQ(info->mcfgStatus, STATUS_OK);
    ASSERT_EQ(info->mcfg.count, 1u);
    ASSERT_EQ(info->mcfg.segs[0].base, 0x4000000000ull);
    ASSERT_EQ(info->mcfg.segs[0].segment, 3u);
    ASSERT_EQ(info->mcfg.segs[0].startBus, 0x10u);
    ASSERT_EQ(info->mcfg.segs[0].endBus, 0x7Fu);
    ASSERT_EQ(info->hpetStatus, STATUS_OK);
    ASSERT_EQ(info->hpet.base, 0x1FED00000ull);
    ASSERT_EQ(info->hpet.blockId, 0x8086A201u);
    ASSERT_EQ(info->hpet.number, 2u);
    ASSERT_EQ(info->hpet.minTick, 0x80u);
    ASSERT_EQ(info->hpet.pageProtection, 1u);
    free(info);
    acpiTablesFree(&ops, s);
    ASSERT_EQ(m.live, 0);
    free(s);
    fakeRelease(&m);
}

/* Every MADT field lands in the right output field: the values are pairwise distinct (in QEMU's
 * MADT a CPU's ACPI UID equals its APIC ID and the I/O APIC's ID and GSI base are 0, so swapping or
 * dropping one of them went unnoticed). */
TEST(acpiMadtFieldsDistinct) {
    static uint8_t t[512];
    uint32_t len = madtStart(t);
    madtLapic(t, &len, 5, 2, 1); /* uid 5, APIC ID 2, enabled */
    madtLapic(t, &len, 6, 9, 2); /* uid 6, APIC ID 9, online-capable */
    uint8_t io[12] = {1, 12, 3}; /* I/O APIC ID 3 */
    put32(io + 4, 0xFEC01000u);
    put32(io + 8, 24);
    madtAdd(t, &len, io, 12);
    uint8_t iso[10] = {2, 10, 1, 9}; /* bus 1, IRQ 9 -> GSI 20, active-low level */
    put32(iso + 4, 20);
    put16(iso + 8, 0xF);
    madtAdd(t, &len, iso, 10);
    uint8_t nmiSrc[8] = {3, 8};
    put16(nmiSrc + 2, 0xD);
    put32(nmiSrc + 4, 7);
    madtAdd(t, &len, nmiSrc, 8);
    uint8_t lnmi[6] = {4, 6, 5};
    put16(lnmi + 3, 0x5);
    lnmi[5] = 0;
    madtAdd(t, &len, lnmi, 6);
    uint8_t x2[16] = {9, 16};
    put32(x2 + 4, 0x200);  /* x2APIC ID */
    put32(x2 + 8, 1);      /* flags */
    put32(x2 + 12, 0x300); /* uid */
    madtAdd(t, &len, x2, 16);
    uint8_t x2nmi[12] = {0xA, 12};
    put16(x2nmi + 2, 0xA);
    put32(x2nmi + 4, 0x77);
    x2nmi[8] = 1;
    madtAdd(t, &len, x2nmi, 12);
    AcpiMadtInfo *m = calloc(1, sizeof(*m));
    ASSERT_EQ(acpiParseMadt(t, len, m), STATUS_OK);
    ASSERT_EQ(m->cpuCount, 3u);
    ASSERT_EQ(m->cpus[0].apicId, 2u);
    ASSERT_EQ(m->cpus[0].uid, 5u);
    ASSERT_EQ(m->cpus[0].flags, 1u);
    ASSERT_TRUE(!m->cpus[0].x2apic);
    ASSERT_EQ(m->cpus[1].apicId, 9u);
    ASSERT_EQ(m->cpus[1].uid, 6u);
    ASSERT_EQ(m->cpus[1].flags, 2u);
    ASSERT_EQ(m->cpus[2].apicId, 0x200u);
    ASSERT_EQ(m->cpus[2].uid, 0x300u);
    ASSERT_EQ(m->cpus[2].flags, 1u);
    ASSERT_TRUE(m->cpus[2].x2apic);
    ASSERT_EQ(m->ioapicCount, 1u);
    ASSERT_EQ(m->ioapics[0].id, 3u);
    ASSERT_EQ(m->ioapics[0].address, 0xFEC01000u);
    ASSERT_EQ(m->ioapics[0].gsiBase, 24u);
    ASSERT_EQ(m->isoCount, 1u);
    ASSERT_EQ(m->isos[0].bus, 1u);
    ASSERT_EQ(m->isos[0].source, 9u);
    ASSERT_EQ(m->isos[0].gsi, 20u);
    ASSERT_EQ(m->isos[0].flags, 0xFu);
    ASSERT_EQ(m->nmiSourceCount, 1u);
    ASSERT_EQ(m->nmiSources[0].flags, 0xDu);
    ASSERT_EQ(m->nmiSources[0].gsi, 7u);
    ASSERT_EQ(m->lapicNmiCount, 2u);
    ASSERT_EQ(m->lapicNmis[0].uid, 5u);
    ASSERT_EQ(m->lapicNmis[0].flags, 0x5u);
    ASSERT_EQ(m->lapicNmis[0].lint, 0u);
    ASSERT_EQ(m->lapicNmis[1].uid, 0x77u);
    ASSERT_EQ(m->lapicNmis[1].flags, 0xAu);
    ASSERT_EQ(m->lapicNmis[1].lint, 1u);
    ASSERT_EQ(m->malformedEntries, 0u);
    free(m);
}

/* Every IVHD field, with a register base above 4 GiB and a 64-bit EFR (type 0x40), plus a type 0x10
 * block, which has no EFR. */
TEST(acpiIvrsIvhdFields) {
    static uint8_t t[256];
    mkTable(t, "IVRS", 48, 2);
    uint32_t len = 48;
    ivrsBlock(t, &len, 0x40, 48);
    uint8_t *b = t + 48;
    b[1] = 0xB0;
    put16(b + 4, 0x0010);
    put16(b + 6, 0x40);
    put64(b + 8, 0x1FEB80000ull);
    put16(b + 16, 2);
    put16(b + 18, 0x1234);
    put32(b + 20, 0xABCD);
    put64(b + 24, 0x1122334455667788ull);
    ivrsBlock(t, &len, 0x10, 24);
    put32(t + 48 + 48 + 20, 0x5555);
    fixSum(t, len);
    AcpiIvrsInfo v;
    ASSERT_EQ(acpiParseIvrs(t, len, &v), STATUS_OK);
    ASSERT_EQ(v.ivhdCount, 2u);
    ASSERT_EQ(v.ivhd[0].type, 0x40u);
    ASSERT_EQ(v.ivhd[0].flags, 0xB0u);
    ASSERT_EQ(v.ivhd[0].deviceId, 0x0010u);
    ASSERT_EQ(v.ivhd[0].capOffset, 0x40u);
    ASSERT_EQ(v.ivhd[0].base, 0x1FEB80000ull);
    ASSERT_EQ(v.ivhd[0].segment, 2u);
    ASSERT_EQ(v.ivhd[0].info, 0x1234u);
    ASSERT_EQ(v.ivhd[0].featOrAttr, 0xABCDu);
    ASSERT_EQ(v.ivhd[0].efr, 0x1122334455667788ull);
    ASSERT_EQ(v.ivhd[1].type, 0x10u);
    ASSERT_EQ(v.ivhd[1].featOrAttr, 0x5555u);
    ASSERT_EQ(v.ivhd[1].efr, 0u);
    ASSERT_EQ(v.malformed, 0u);
}
