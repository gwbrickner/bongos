/* IOAPIC driver (ARCHITECTURE §7.3, D-172): maps each IOAPIC the MADT lists through a UC MMIO
 * window, masks every pin, and reads/writes redirection entries. Policy (which pin gets which
 * vector, ISA overrides, vector ownership) is irq.c's; the encodings are irq-core.c's. */
#include "include/apic.h"

#include "include/irq-core.h"

#include "klog.h"
#include "vmm.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stdint.h>

#define IOAPIC_REG_ID       0x00u
#define IOAPIC_REG_VER      0x01u
#define IOAPIC_REG_REDIR    0x10u /* entry p: low dword 0x10 + 2p, high dword 0x11 + 2p */
#define IOAPIC_MAX_PINS     120u  /* (0xFF - 0x10 + 1) / 2 */
#define IOAPIC_MMIO_SIZE    0x50u /* IOREGSEL 0x00, IOWIN 0x10, EOI 0x40 (version >= 0x20) */
#define IOAPIC_RTE_MASK_BIT (1u << 16)

typedef struct {
    volatile uint8_t *mmio;
    uint8_t id;
} Ioapic;

static Ioapic ioapics[ACPI_MAX_IOAPICS];
static IrqGsiRange ranges[ACPI_MAX_IOAPICS]; /* parallel to ioapics[] */
static uint32_t count = 0;
static uint32_t reservedGsi[ACPI_MAX_NMI_SOURCES];
static uint32_t reservedCount = 0;

/* Index/data access, serialized by the caller's IRQ-disable (the pair must not be interleaved with
 * another access to the same IOAPIC). */
static uint32_t regRead(const Ioapic *io, uint32_t reg) {
    *(volatile uint32_t *)(io->mmio + 0x00) = reg;
    return *(volatile uint32_t *)(io->mmio + 0x10);
}

static void regWrite(const Ioapic *io, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(io->mmio + 0x00) = reg;
    *(volatile uint32_t *)(io->mmio + 0x10) = value;
}

void ioapicInitAll(const AcpiMadtInfo *madt) {
    if (madt == NULL) {
        return;
    }
    for (uint32_t i = 0; i < madt->ioapicCount && count < ACPI_MAX_IOAPICS; i++) {
        const AcpiIoApic *a = &madt->ioapics[i];
        if ((a->address & 0xFu) != 0) {
            klogWrite(KLOG_ERROR, "ioapic", "id %u: misaligned address 0x%08x, skipped", a->id,
                      a->address);
            continue;
        }
        volatile void *va;
        Status st = vmmMapMmio(a->address, IOAPIC_MMIO_SIZE, &va);
        if (st != STATUS_OK) {
            klogWrite(KLOG_ERROR, "ioapic", "id %u: cannot map 0x%08x (status %d), skipped", a->id,
                      a->address, (int)st);
            continue;
        }
        Ioapic io = {.mmio = (volatile uint8_t *)va, .id = a->id};
        uint64_t f = archIrqSave();
        uint32_t idReg = regRead(&io, IOAPIC_REG_ID);
        uint32_t ver = regRead(&io, IOAPIC_REG_VER);
        archIrqRestore(f);
        IrqGsiRange range = {a->gsiBase, irqCoreIoapicPins(ver)};
        if (range.pins > IOAPIC_MAX_PINS) {
            /* IOREGSEL is 8 bits: entry p occupies registers 0x10+2p and 0x11+2p, so only 120
             * entries are addressable. A larger count is a garbage version register. */
            klogWrite(KLOG_WARN, "ioapic", "id %u: version register claims %u pins; using %u",
                      a->id, range.pins, IOAPIC_MAX_PINS);
            range.pins = IOAPIC_MAX_PINS;
        }
        if (irqCoreGsiOverlaps(ranges, count, range)) {
            klogWrite(KLOG_ERROR, "ioapic", "id %u: GSIs %u-%u overlap an earlier IOAPIC, skipped",
                      a->id, range.gsiBase, range.gsiBase + range.pins - 1);
            vmmUnmapMmio(va, IOAPIC_MMIO_SIZE);
            continue;
        }
        if ((idReg >> 24) != a->id) {
            klogWrite(KLOG_WARN, "ioapic", "id register says %u, MADT says %u", idReg >> 24, a->id);
        }
        f = archIrqSave();
        for (uint32_t p = 0; p < range.pins; p++) {
            regWrite(&io, IOAPIC_REG_REDIR + 2 * p, IOAPIC_RTE_MASK_BIT);
            regWrite(&io, IOAPIC_REG_REDIR + 2 * p + 1, 0);
        }
        archIrqRestore(f);
        ioapics[count] = io;
        ranges[count] = range;
        count++;
        klogWrite(KLOG_INFO, "ioapic", "id=%u addr=0x%08x version=0x%02x pins=%u gsi=%u-%u", a->id,
                  a->address, ver & 0xFFu, range.pins, range.gsiBase,
                  range.gsiBase + range.pins - 1);
    }
    for (uint32_t i = 0; i < madt->nmiSourceCount && reservedCount < ACPI_MAX_NMI_SOURCES; i++) {
        reservedGsi[reservedCount++] = madt->nmiSources[i].gsi;
        klogWrite(KLOG_INFO, "ioapic", "GSI %u reserved (MADT NMI source), left masked",
                  madt->nmiSources[i].gsi);
    }
}

uint32_t ioapicCount(void) {
    return count;
}

bool ioapicInfo(uint32_t index, uint32_t *gsiBase, uint32_t *pins) {
    if (index >= count) {
        return false;
    }
    *gsiBase = ranges[index].gsiBase;
    *pins = ranges[index].pins;
    return true;
}

bool ioapicGsiUsable(uint32_t gsi) {
    if (irqCoreGsiLookup(ranges, count, gsi, NULL) < 0) {
        return false;
    }
    for (uint32_t i = 0; i < reservedCount; i++) {
        if (reservedGsi[i] == gsi) {
            return false;
        }
    }
    return true;
}

static const Ioapic *findPin(uint32_t gsi, uint32_t *outPin) {
    int32_t i = irqCoreGsiLookup(ranges, count, gsi, outPin);
    return i < 0 ? NULL : &ioapics[i];
}

Status ioapicWriteRte(uint32_t gsi, uint64_t rte) {
    uint32_t pin;
    const Ioapic *io = findPin(gsi, &pin);
    if (io == NULL) {
        return STATUS_ERR_NOT_FOUND;
    }
    uint64_t f = archIrqSave();
    regWrite(io, IOAPIC_REG_REDIR + 2 * pin, (uint32_t)rte | IOAPIC_RTE_MASK_BIT);
    regWrite(io, IOAPIC_REG_REDIR + 2 * pin + 1, (uint32_t)(rte >> 32));
    regWrite(io, IOAPIC_REG_REDIR + 2 * pin, (uint32_t)rte);
    archIrqRestore(f);
    return STATUS_OK;
}

Status ioapicSetMask(uint32_t gsi, bool masked) {
    uint32_t pin;
    const Ioapic *io = findPin(gsi, &pin);
    if (io == NULL) {
        return STATUS_ERR_NOT_FOUND;
    }
    uint64_t f = archIrqSave();
    uint32_t lo = regRead(io, IOAPIC_REG_REDIR + 2 * pin);
    lo = masked ? (lo | IOAPIC_RTE_MASK_BIT) : (lo & ~IOAPIC_RTE_MASK_BIT);
    regWrite(io, IOAPIC_REG_REDIR + 2 * pin, lo);
    archIrqRestore(f);
    return STATUS_OK;
}

Status ioapicReadRte(uint32_t gsi, uint64_t *out) {
    uint32_t pin;
    const Ioapic *io = findPin(gsi, &pin);
    if (io == NULL) {
        return STATUS_ERR_NOT_FOUND;
    }
    uint64_t f = archIrqSave();
    uint32_t lo = regRead(io, IOAPIC_REG_REDIR + 2 * pin);
    uint32_t hi = regRead(io, IOAPIC_REG_REDIR + 2 * pin + 1);
    archIrqRestore(f);
    *out = ((uint64_t)hi << 32) | lo;
    return STATUS_OK;
}
