/* The pure, host-tested core of the interrupt-controller code (ARCHITECTURE §7.2/§7.3, D-172,
 * D-174, ROADMAP M3.2): the vector allocator's bitmap, IOAPIC redirection-entry encoding, ISA IRQ
 * -> GSI resolution through the MADT interrupt source overrides, and GSI -> IOAPIC lookup. No
 * klog, panic or assembly, no locks (callers serialize): everything here is reentrant over
 * caller-owned state. The hardware side is kernel/arch/x86_64/{pic8259,lapic,ioapic,irq}.c. */
#ifndef KERNEL_ARCH_X86_64_IRQ_CORE_H
#define KERNEL_ARCH_X86_64_IRQ_CORE_H

#include "acpi-tables.h"
#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

#define IRQ_VECTOR_FIRST_DYNAMIC 48u /* ARCHITECTURE §7.2: 48-239 are device IRQs */
#define IRQ_VECTOR_LAST_DYNAMIC  239u
#define IRQ_VECTOR_COUNT         256u
#define IRQ_ISA_COUNT            16u

/* A 256-bit bitmap; a set bit means "not available to irqVectorMapAlloc". */
typedef struct {
    uint64_t used[4];
} IrqVectorMap;

/* Marks 0-47 (exceptions and the remapped 8259) and 240-255 (IPIs, LAPIC timer, spurious) used
 * and 48-239 free. */
void irqVectorMapInit(IrqVectorMap *m);

/* Allocates the lowest free vector in [48, 239] into *out. STATUS_ERR_NO_MEMORY if none is free. */
Status irqVectorMapAlloc(IrqVectorMap *m, uint32_t *out);

/* Frees an allocated dynamic vector. STATUS_ERR_INVALID if `v` is outside [48, 239] or not
 * currently allocated (a double free), leaving the map unchanged. */
Status irqVectorMapFree(IrqVectorMap *m, uint32_t v);

/* True iff `v` is in [48, 239] and currently allocated. */
bool irqVectorMapIsAllocated(const IrqVectorMap *m, uint32_t v);

/* How many vectors in [48, 239] are currently allocated. */
uint32_t irqVectorMapAllocatedCount(const IrqVectorMap *m);

typedef struct {
    uint32_t gsi;
    bool activeLow, level;
} IrqIsaRoute;

/* Resolves legacy ISA IRQ `irq` (0-15) to its GSI, polarity and trigger from the MADT interrupt
 * source overrides `isos[0..n)`. The ISA bus default is edge-triggered, active-high. MPS INTI
 * flags: polarity bits 1:0 (00 conforms to the bus, 01 high, 11 low, 10 reserved), trigger bits
 * 3:2 (00 conforms, 01 edge, 11 level, 10 reserved); a reserved encoding conforms to the bus and
 * sets *outWarn. Overrides with bus != 0 or source > 15 are ignored (*outWarn). The first override
 * per source wins; a later duplicate, or a later override targeting a GSI an earlier accepted one
 * already claimed, is ignored (*outWarn). With no override for `irq` the GSI is `irq` itself,
 * unless some other accepted override targets that GSI (the pin is taken over by another source,
 * e.g. ISA IRQ 2 when IRQ 0 is overridden to GSI 2), which is STATUS_ERR_NOT_FOUND.
 * STATUS_ERR_INVALID if irq > 15 or an argument is NULL. `outWarn` may be NULL. Pure. */
Status irqCoreIsaRoute(const AcpiIso *isos, uint32_t n, uint32_t irq, IrqIsaRoute *out,
                       bool *outWarn);

/* The 64-bit IOAPIC redirection entry (82093AA §3.2.4): vector in bits 7:0, delivery mode Fixed,
 * physical destination mode, polarity (bit 13, 1 = active low), trigger (bit 15, 1 = level), mask
 * (bit 16), destination APIC ID in bits 63:56. Pure. */
uint64_t irqCoreRteEncode(uint8_t vector, bool activeLow, bool level, bool masked, uint8_t dest);

/* Number of redirection entries from the IOAPIC version register: ((v >> 16) & 0xFF) + 1. */
uint32_t irqCoreIoapicPins(uint32_t versionReg);

typedef struct {
    uint32_t gsiBase, pins;
} IrqGsiRange;

/* Index of the range in r[0..n) containing `gsi` (64-bit math, so base + pins never overflows),
 * with *outPin = gsi - base; -1 if none. */
int32_t irqCoreGsiLookup(const IrqGsiRange *r, uint32_t n, uint32_t gsi, uint32_t *outPin);

/* True iff `cand` shares a GSI with any range in r[0..n), or has zero pins. */
bool irqCoreGsiOverlaps(const IrqGsiRange *r, uint32_t n, IrqGsiRange cand);

#endif
