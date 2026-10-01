/* Host tests for the ACPI parsers against QEMU's real tables (M3.1, ROADMAP Done-when: "the host
 * tests pass" over the stored tables). tests/data/acpi/qemu-q35/{uefi,bios}/ are tools/acpiextract
 * outputs of a `-M q35 -smp 4 -m 512` boot (README there has the provenance): UEFI (OVMF) hands
 * the kernel an XSDT, BIOS (SeaBIOS) a revision-0 RSDP with only an RSDT, with different MCFG
 * bases. The fixtures are frozen: assertions use semantic fields only, never hashes, checksums or
 * addresses that depend on where firmware happened to allocate. */
#include "acpi-dump.h"
#include "acpi-tables.h"
#include "acpi_fixture.h"
#include "acpiextract.h"
#include "framework/test.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *dir;
    bool xsdt;
    uint64_t mcfgBase;
} FwCase;

static const FwCase uefiCase = {"tests/data/acpi/qemu-q35/uefi", true, 0xE0000000ull};
static const FwCase biosCase = {"tests/data/acpi/qemu-q35/bios", false, 0xB0000000ull};

typedef struct {
    AcpiFx fx;
    AcpiPhysOps ops;
    AcpiTableSet set;
    AcpiInfo info;
} Loaded;

static Loaded *loadFw(const FwCase *c) {
    Loaded *l = calloc(1, sizeof(*l));
    if (l == NULL || acpiFxLoad(&l->fx, c->dir) != 0) {
        fprintf(stderr, "  cannot load fixture %s (run from the repo root)\n", c->dir);
        hostTestFailures++;
        return NULL;
    }
    l->ops = acpiFxOps(&l->fx);
    if (acpiTablesLoad(&l->ops, l->fx.rsdpPhys, &l->set) != STATUS_OK) {
        fprintf(stderr, "  acpiTablesLoad failed for %s\n", c->dir);
        hostTestFailures++;
        acpiFxRelease(&l->fx);
        free(l);
        return NULL;
    }
    acpiParseAll(&l->set, &l->info);
    return l;
}

static void unload(Loaded *l) {
    acpiTablesFree(&l->ops, &l->set);
    int live = l->fx.live;
    acpiFxRelease(&l->fx);
    free(l);
    if (live != 0) {
        fprintf(stderr, "  leaked %d table copies\n", live);
        hostTestFailures++;
    }
}

static void checkDiscovery(const FwCase *c) {
    Loaded *l = loadFw(c);
    if (l == NULL) {
        return;
    }
    const AcpiTableSet *s = &l->set;
    ASSERT_EQ(s->usedXsdt, c->xsdt);
    ASSERT_EQ(s->rsdpRevision >= 2, c->xsdt);
    ASSERT_EQ(s->rejected, 0u);
    ASSERT_EQ(s->dropped, 0u);
    ASSERT_EQ(s->warnings, 0u);
    ASSERT_TRUE(acpiTablesFind(s, "FACP", 0) != NULL);
    ASSERT_TRUE(acpiTablesFind(s, "APIC", 0) != NULL);
    ASSERT_TRUE(acpiTablesFind(s, "MCFG", 0) != NULL);
    ASSERT_TRUE(acpiTablesFind(s, "HPET", 0) != NULL);
    ASSERT_TRUE(s->dsdtIndex >= 0);
    ASSERT_TRUE(s->fadtIndex >= 0);
    ASSERT_EQ(s->tables[s->dsdtIndex].phys, l->info.fadt.dsdtPhys);
    ASSERT_TRUE(memcmp(s->tables[s->dsdtIndex].signature, "DSDT", 4) == 0);
    for (uint32_t i = 0; i < s->count; i++) {
        ASSERT_EQ(acpiChecksum(s->tables[i].data, s->tables[i].length), 0u);
        ASSERT_EQ(acpiRd32(s->tables[i].data + 4), s->tables[i].length);
    }
    /* Every manifest table (but the RSDP) was loaded, nothing more. */
    ASSERT_EQ(s->count, (uint32_t)l->fx.count - 1);
    unload(l);
}

static void checkMadt(const FwCase *c) {
    Loaded *l = loadFw(c);
    if (l == NULL) {
        return;
    }
    const AcpiMadtInfo *m = &l->info.madt;
    ASSERT_EQ(l->info.madtStatus, STATUS_OK);
    ASSERT_EQ(m->cpuCount, 4u);
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_EQ(m->cpus[i].apicId, i);
        ASSERT_EQ(m->cpus[i].uid, i);
        ASSERT_TRUE((m->cpus[i].flags & 1) != 0);
        ASSERT_TRUE(!m->cpus[i].x2apic);
    }
    ASSERT_EQ(m->cpusDisabled, 0u);
    ASSERT_EQ(m->lapicAddress, (uint64_t)0xFEE00000);
    ASSERT_TRUE(m->pcatCompat);
    ASSERT_EQ(m->ioapicCount, 1u);
    ASSERT_EQ(m->ioapics[0].address, 0xFEC00000u);
    ASSERT_EQ(m->ioapics[0].gsiBase, 0u);
    bool irq0to2 = false, level9 = false, nmiAll = false;
    for (uint32_t i = 0; i < m->isoCount; i++) {
        irq0to2 = irq0to2 || (m->isos[i].source == 0 && m->isos[i].gsi == 2);
        level9 = level9 || (m->isos[i].source == 9 && (m->isos[i].flags & 3) == 1 &&
                            ((m->isos[i].flags >> 2) & 3) == 3);
    }
    for (uint32_t i = 0; i < m->lapicNmiCount; i++) {
        nmiAll = nmiAll || (m->lapicNmis[i].uid == ACPI_LAPIC_NMI_ALL && m->lapicNmis[i].lint == 1);
    }
    ASSERT_TRUE(irq0to2);
    ASSERT_TRUE(level9);
    ASSERT_TRUE(nmiAll);
    ASSERT_EQ(m->malformedEntries, 0u);
    unload(l);
}

static void checkFadt(const FwCase *c) {
    Loaded *l = loadFw(c);
    if (l == NULL) {
        return;
    }
    const AcpiFadtInfo *f = &l->info.fadt;
    ASSERT_EQ(l->info.fadtStatus, STATUS_OK);
    ASSERT_EQ(f->sciInt, 9u);
    ASSERT_EQ(f->pm1aEvt.spaceId, 1u);
    ASSERT_EQ(f->pm1aCnt.address, f->pm1aEvt.address + 4);
    ASSERT_EQ(f->pmTmr.address, f->pm1aEvt.address + 8);
    ASSERT_TRUE(f->pmTimerPresent);
    ASSERT_TRUE(!f->hwReduced);
    ASSERT_TRUE(f->smiCmd != 0);
    ASSERT_TRUE(f->dsdtPhys != 0);
    ASSERT_TRUE(!f->dsdtMismatch);
    unload(l);
}

static void checkMcfgHpet(const FwCase *c) {
    Loaded *l = loadFw(c);
    if (l == NULL) {
        return;
    }
    ASSERT_EQ(l->info.mcfgStatus, STATUS_OK);
    ASSERT_EQ(l->info.mcfg.count, 1u);
    ASSERT_EQ(l->info.mcfg.segs[0].base, c->mcfgBase);
    ASSERT_EQ(l->info.mcfg.segs[0].segment, 0u);
    ASSERT_EQ(l->info.mcfg.segs[0].startBus, 0u);
    ASSERT_EQ(l->info.mcfg.segs[0].endBus, 255u);
    ASSERT_EQ(l->info.hpetStatus, STATUS_OK);
    ASSERT_EQ(l->info.hpet.base, (uint64_t)0xFED00000);
    ASSERT_EQ(l->info.hpetTableCount, 1u);
    ASSERT_EQ(l->info.ivrsStatus, STATUS_ERR_NOT_FOUND); /* no AMD IOMMU in this configuration */
    unload(l);
}

typedef struct {
    char *text;
    size_t len, cap;
} Sink;

static void sinkLine(void *ctx, const char *line) {
    Sink *s = ctx;
    size_t n = strlen(line);
    while (s->len + n + 2 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4096;
        s->text = realloc(s->text, s->cap);
    }
    memcpy(s->text + s->len, line, n);
    s->len += n;
    s->text[s->len++] = '\n';
}

/* dump -> acpiextract parse reproduces every stored table byte for byte, RSDP included. */
static void checkDumpRoundTrip(const FwCase *c) {
    Loaded *l = loadFw(c);
    if (l == NULL) {
        return;
    }
    Sink sink = {0};
    acpiDumpTables(&l->set, sinkLine, &sink);
    AcpiExtractDump d;
    char err[256];
    ASSERT_EQ(acpiExtractParse(sink.text, sink.len, &d, err, sizeof(err)), 0);
    ASSERT_EQ(d.rsdp, l->fx.rsdpPhys);
    ASSERT_EQ(d.count, (uint32_t)l->fx.count);
    for (uint32_t i = 0; i < d.count; i++) {
        const AcpiFxBlock *b = &l->fx.blocks[i];
        ASSERT_EQ(d.tables[i].len, b->len);
        ASSERT_EQ(d.tables[i].phys, b->phys);
        ASSERT_TRUE(memcmp(d.tables[i].data, b->bytes, b->len) == 0);
        ASSERT_TRUE(strcmp(d.tables[i].sig, b->sig) == 0);
    }
    acpiExtractFree(&d);
    free(sink.text);
    unload(l);
}

static uint64_t rngState;
static uint32_t rnd(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return (uint32_t)(rngState >> 16);
}

static Status parseBySig(const char *sig, const uint8_t *t, uint32_t len) {
    static AcpiInfo scratch;
    if (strcmp(sig, "FACP") == 0) {
        return acpiParseFadt(t, len, &scratch.fadt);
    }
    if (strcmp(sig, "APIC") == 0) {
        return acpiParseMadt(t, len, &scratch.madt);
    }
    if (strcmp(sig, "MCFG") == 0) {
        return acpiParseMcfg(t, len, &scratch.mcfg);
    }
    if (strcmp(sig, "HPET") == 0) {
        return acpiParseHpet(t, len, &scratch.hpet);
    }
    return STATUS_OK;
}

/* Every truncation, then random byte flips (Length field kept consistent so the mutation reaches
 * the parse logic): the parsers return a known status and ASan/UBSan stay quiet. */
static void checkParserFuzz(const FwCase *c) {
    Loaded *l = loadFw(c);
    if (l == NULL) {
        return;
    }
    rngState = 0x9E3779B97F4A7C15ull;
    static const char *sigs[] = {"FACP", "APIC", "MCFG", "HPET"};
    for (unsigned si = 0; si < 4; si++) {
        const AcpiTable *t = acpiTablesFind(&l->set, sigs[si], 0);
        ASSERT_TRUE(t != NULL);
        for (uint32_t cut = 0; cut <= t->length; cut++) {
            /* A table of exactly `cut` heap bytes (so ASan sees any read past `cut`), first with
             * the Length field still saying full, then with it saying `cut`. */
            uint8_t *cutBuf = malloc(cut == 0 ? 1 : cut);
            memcpy(cutBuf, t->data, cut);
            Status st = parseBySig(sigs[si], cutBuf, cut);
            ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID);
            if (cut >= 8) {
                cutBuf[4] = (uint8_t)cut;
                cutBuf[5] = (uint8_t)(cut >> 8);
                cutBuf[6] = (uint8_t)(cut >> 16);
                cutBuf[7] = 0;
                st = parseBySig(sigs[si], cutBuf, cut);
                ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID);
            }
            free(cutBuf);
        }
        uint8_t *buf = malloc(t->length);
        for (int iter = 0; iter < 1500; iter++) {
            memcpy(buf, t->data, t->length);
            int flips = 1 + (int)(rnd() % 4);
            for (int f = 0; f < flips; f++) {
                uint32_t pos = 8 + rnd() % (t->length - 8); /* keep signature and Length */
                buf[pos] = (uint8_t)rnd();
            }
            Status st = parseBySig(sigs[si], buf, t->length);
            ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID);
        }
        free(buf);
    }
    unload(l);
}

/* Loader fuzz: mutate bytes of the stored RSDP, root and children in the fake memory and reload.
 * The only assertions: a known status, and every allocation freed (live == 0). */
static void checkLoaderFuzz(const FwCase *c) {
    rngState = 0xD1B54A32D192ED03ull;
    for (int iter = 0; iter < 600; iter++) {
        AcpiFx fx;
        if (acpiFxLoad(&fx, c->dir) != 0) {
            hostTestFailures++;
            return;
        }
        int flips = 1 + (int)(rnd() % 3);
        for (int f = 0; f < flips; f++) {
            AcpiFxBlock *b = &fx.blocks[rnd() % (uint32_t)fx.count];
            b->bytes[rnd() % b->len] = (uint8_t)rnd();
        }
        AcpiPhysOps ops = acpiFxOps(&fx);
        AcpiTableSet *s = calloc(1, sizeof(*s));
        Status st = acpiTablesLoad(&ops, fx.rsdpPhys, s);
        ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID || st == STATUS_ERR_NOT_FOUND);
        if (st != STATUS_OK) {
            ASSERT_EQ(s->count, 0u);
        }
        AcpiInfo *info = calloc(1, sizeof(*info));
        if (st == STATUS_OK) {
            acpiParseAll(s, info);
        }
        acpiTablesFree(&ops, s);
        ASSERT_EQ(fx.live, 0);
        free(info);
        free(s);
        acpiFxRelease(&fx);
    }
}

TEST(acpiStoredDiscoveryUefi) {
    checkDiscovery(&uefiCase);
}
TEST(acpiStoredDiscoveryBios) {
    checkDiscovery(&biosCase);
}
TEST(acpiStoredMadtUefi) {
    checkMadt(&uefiCase);
}
TEST(acpiStoredMadtBios) {
    checkMadt(&biosCase);
}
TEST(acpiStoredFadtUefi) {
    checkFadt(&uefiCase);
}
TEST(acpiStoredFadtBios) {
    checkFadt(&biosCase);
}
TEST(acpiStoredMcfgHpetUefi) {
    checkMcfgHpet(&uefiCase);
}
TEST(acpiStoredMcfgHpetBios) {
    checkMcfgHpet(&biosCase);
}
TEST(acpiStoredDumpRoundTripUefi) {
    checkDumpRoundTrip(&uefiCase);
}
TEST(acpiStoredDumpRoundTripBios) {
    checkDumpRoundTrip(&biosCase);
}
TEST(acpiStoredParserFuzzUefi) {
    checkParserFuzz(&uefiCase);
}
TEST(acpiStoredParserFuzzBios) {
    checkParserFuzz(&biosCase);
}
TEST(acpiStoredLoaderFuzzUefi) {
    checkLoaderFuzz(&uefiCase);
}
TEST(acpiStoredLoaderFuzzBios) {
    checkLoaderFuzz(&biosCase);
}
