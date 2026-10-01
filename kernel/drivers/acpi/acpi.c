/* Kernel-side ACPI glue (D-166..D-168): the AcpiPhysOps for the pure loader (temporary read-only
 * KVA mappings, kmalloc/vmalloc copies), the boot-time log lines `mk/test.mk` greps, and the
 * `acpidump=1` serial dump. See kernel/include/acpi.h. */
#include "acpi.h"

#include "acpi-dump.h"
#include "cmdline.h"
#include "klog.h"
#include "kmalloc.h"
#include "panic.h"
#include "vmalloc.h"
#include "vmm.h"

#include "drivers/serial/uart16550.h"

#include <stdbool.h>

typedef struct {
    const BootMemRegion *map;
    uint32_t mapCount;
} AcpiKernelCtx;

static AcpiTableSet acpiTableSet;
static AcpiInfo acpiInfo;
static bool acpiReady;

/* Copies [phys, phys+len) out of firmware memory through a temporary read-only WB KVA mapping
 * (the HHDM does not map RESERVED memory, where BIOS keeps the RSDP and every table). A range
 * outside acpiPhysRangeAllowed()'s policy is refused. The mapping lives only for the copy. */
static Status acpiReadPhys(void *ctx, uint64_t phys, void *dst, uint32_t len) {
    const AcpiKernelCtx *k = ctx;
    if (len == 0) {
        return STATUS_OK;
    }
    if (!acpiPhysRangeAllowed(k->map, k->mapCount, phys, len)) {
        return STATUS_ERR_INVALID;
    }
    uint64_t pageBase = phys & ~(uint64_t)0xFFF;
    uint64_t pageEnd = (phys + len + 0xFFF) & ~(uint64_t)0xFFF;
    uint64_t size = pageEnd - pageBase;
    uint64_t va;
    Status st = vmmKvaAlloc(size, &va);
    if (st != STATUS_OK) {
        return st;
    }
    st = vmmMapKernel(va, pageBase, size, VMM_CACHE_WB); /* no VMM_WRITE: read-only */
    if (st != STATUS_OK) {
        vmmKvaFree(va, size);
        return st;
    }
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)(va + (phys - pageBase));
    uint8_t *out = dst;
    for (uint32_t i = 0; i < len; i++) {
        out[i] = src[i];
    }
    if (vmmUnmapKernel(va, size) != STATUS_OK) {
        panic("acpi: could not unmap a temporary table window at 0x%llx", (unsigned long long)va);
    }
    vmmKvaFree(va, size);
    return STATUS_OK;
}

/* kmalloc covers up to KMALLOC_MAX_SIZE; anything bigger goes to vmalloc. The free side makes the
 * same choice from the same length, so the pair always matches. */
static void *acpiAlloc(void *ctx, uint32_t len) {
    (void)ctx;
    return len <= KMALLOC_MAX_SIZE ? kmalloc(len, 0) : vmalloc(len, 0);
}

static void acpiFree(void *ctx, void *p, uint32_t len) {
    (void)ctx;
    if (len <= KMALLOC_MAX_SIZE) {
        kfree(p);
    } else {
        vfree(p);
    }
}

static void sigToString(const char sig[4], char out[5]) {
    for (int i = 0; i < 4; i++) {
        char c = sig[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        out[i] = ok ? c : '_';
    }
    out[4] = '\0';
}

static void dumpEmit(void *ctx, const char *line) {
    (void)ctx;
    serialWriteString(line);
    serialWriteString("\n");
}

static void logTables(const AcpiTableSet *s) {
    char oem[7];
    for (int i = 0; i < 6; i++) {
        char c = s->oemId[i];
        oem[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    oem[6] = '\0';
    klogWrite(KLOG_INFO, "acpi", "RSDP 0x%016llx rev %u oem \"%s\" -> %s 0x%016llx",
              (unsigned long long)s->rsdpPhys, (unsigned)s->rsdpRevision, oem,
              s->usedXsdt ? "XSDT" : "RSDT",
              (unsigned long long)(s->usedXsdt ? s->xsdtPhys : s->rsdtPhys));
    for (uint32_t i = 0; i < s->count; i++) {
        const AcpiTable *t = &s->tables[i];
        char sig[5];
        sigToString(t->signature, sig);
        klogWrite(KLOG_INFO, "acpi", "%s 0x%016llx len %u rev %u", sig, (unsigned long long)t->phys,
                  t->length, (unsigned)t->revision);
    }
    for (uint32_t i = 0; i < s->rejected && i < ACPI_MAX_REJECTS; i++) {
        char sig[5];
        sigToString(s->rejects[i].signature, sig);
        klogWrite(KLOG_WARN, "acpi", "rejected table at 0x%016llx (sig %s, status %d)",
                  (unsigned long long)s->rejects[i].phys, sig, (int)s->rejects[i].status);
    }
    if (s->rejected > ACPI_MAX_REJECTS) {
        klogWrite(KLOG_WARN, "acpi", "%u more rejected tables not listed",
                  s->rejected - ACPI_MAX_REJECTS);
    }
    static const struct {
        uint32_t bit;
        const char *text;
    } warnings[] = {
        {ACPI_WARN_RSDP_V2_BAD, "RSDP v2 extension is invalid; using the v1 fields"},
        {ACPI_WARN_XSDT_FALLBACK, "XSDT unusable; fell back to the RSDT"},
        {ACPI_WARN_ROOT_TRAILING, "root table has trailing bytes after the last entry"},
        {ACPI_WARN_TABLES_DROPPED, "too many tables; some were dropped"},
        {ACPI_WARN_DSDT_MISMATCH, "FADT DSDT and X_DSDT differ; using X_DSDT"},
        {ACPI_WARN_NO_DSDT, "no DSDT"},
        {ACPI_WARN_DUPLICATE_ENTRY, "root table lists a table twice"},
    };
    for (unsigned i = 0; i < sizeof(warnings) / sizeof(warnings[0]); i++) {
        if ((s->warnings & warnings[i].bit) != 0) {
            klogWrite(KLOG_WARN, "acpi", "%s", warnings[i].text);
        }
    }
}

static void logInfo(const AcpiInfo *a) {
    if (a->fadtStatus == STATUS_OK) {
        const AcpiFadtInfo *f = &a->fadt;
        klogWrite(KLOG_INFO, "acpi", "FADT sci=%u pm-timer=0x%llx (%u-bit) reset=%s dsdt=0x%016llx",
                  (unsigned)f->sciInt, (unsigned long long)f->pmTmr.address,
                  f->pmTimerPresent ? (f->pmTimer32Bit ? 32u : 24u) : 0u,
                  f->resetSupported ? "yes" : "no", (unsigned long long)f->dsdtPhys);
    } else {
        klogWrite(KLOG_WARN, "acpi", "no usable FADT (status %d)", (int)a->fadtStatus);
    }
    if (a->madtStatus == STATUS_OK) {
        const AcpiMadtInfo *m = &a->madt;
        klogWrite(KLOG_INFO, "acpi", "MADT lapic=0x%016llx pcat=%u cpus=%u ioapics=%u overrides=%u",
                  (unsigned long long)m->lapicAddress, m->pcatCompat ? 1u : 0u, m->cpuCount,
                  m->ioapicCount, m->isoCount);
        for (uint32_t i = 0; i < m->cpuCount; i++) {
            klogWrite(KLOG_INFO, "acpi", "MADT cpu apic-id=%u uid=%u %s%s", m->cpus[i].apicId,
                      m->cpus[i].uid, (m->cpus[i].flags & 1) != 0 ? "enabled" : "online-capable",
                      m->cpus[i].x2apic ? " x2apic" : "");
        }
        for (uint32_t i = 0; i < m->ioapicCount; i++) {
            klogWrite(KLOG_INFO, "acpi", "MADT ioapic id=%u addr=0x%08x gsi-base=%u",
                      (unsigned)m->ioapics[i].id, m->ioapics[i].address, m->ioapics[i].gsiBase);
        }
        for (uint32_t i = 0; i < m->isoCount; i++) {
            klogWrite(KLOG_INFO, "acpi", "MADT override irq=%u gsi=%u flags=0x%04x",
                      (unsigned)m->isos[i].source, m->isos[i].gsi, (unsigned)m->isos[i].flags);
        }
    } else {
        klogWrite(KLOG_WARN, "acpi", "no usable MADT (status %d)", (int)a->madtStatus);
    }
    if (a->mcfgStatus == STATUS_OK) {
        for (uint32_t i = 0; i < a->mcfg.count; i++) {
            const AcpiMcfgSeg *s = &a->mcfg.segs[i];
            klogWrite(KLOG_INFO, "acpi", "MCFG base=0x%016llx seg=%u bus=%u-%u",
                      (unsigned long long)s->base, (unsigned)s->segment, (unsigned)s->startBus,
                      (unsigned)s->endBus);
        }
    } else {
        klogWrite(KLOG_WARN, "acpi", "no usable MCFG (status %d)", (int)a->mcfgStatus);
    }
    if (a->hpetStatus == STATUS_OK) {
        klogWrite(KLOG_INFO, "acpi", "HPET base=0x%016llx", (unsigned long long)a->hpet.base);
    }
    if (a->ivrsStatus == STATUS_OK) {
        klogWrite(KLOG_INFO, "acpi", "IVRS %u IVHD block(s)", a->ivrs.ivhdCount);
    }
}

Status acpiInit(const BootInfo *bi, const BootMemRegion *map, uint32_t mapCount,
                const char *cmdline) {
    AcpiKernelCtx ctx = {map, mapCount};
    AcpiPhysOps ops = {&ctx, acpiReadPhys, acpiAlloc, acpiFree};
    Status st = acpiTablesLoad(&ops, bi->rsdpPhys, &acpiTableSet);
    if (st == STATUS_ERR_NOT_FOUND) {
        klogWrite(KLOG_WARN, "acpi", "the loader reported no RSDP");
        return st;
    }
    if (st != STATUS_OK) {
        klogWrite(KLOG_ERROR, "acpi", "no usable tables (status %d)", (int)st);
        return st;
    }
    logTables(&acpiTableSet);
    acpiParseAll(&acpiTableSet, &acpiInfo);
    logInfo(&acpiInfo);
    if (cmdlineHasToken(cmdline, "acpidump=1")) {
        acpiDumpTables(&acpiTableSet, dumpEmit, NULL);
    }
    acpiReady = true;
    return STATUS_OK;
}

const AcpiInfo *acpiGetInfo(void) {
    return acpiReady ? &acpiInfo : NULL;
}

const AcpiTableSet *acpiGetTables(void) {
    return acpiReady ? &acpiTableSet : NULL;
}

const AcpiTable *acpiFindTable(const char sig[4], uint32_t instance) {
    return acpiReady ? acpiTablesFind(&acpiTableSet, sig, instance) : NULL;
}
