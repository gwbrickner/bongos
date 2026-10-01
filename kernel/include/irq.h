/* Interrupt routing and dispatch (ARCHITECTURE §7.2/§7.3, D-172..D-174, ROADMAP M3.2): the vector
 * allocator, handler registration, and IOAPIC routing of legacy ISA IRQs and GSIs. The hardware
 * (8259, local APIC, IOAPIC) lives under kernel/arch/x86_64/.
 *
 * Handler rules (D-173): a handler runs with IF=0 on the interrupted context's stack, is never
 * nested, must not sleep, must not call the allocators (pmm, slab, kmalloc, vmm, vmalloc) until the
 * real locks of M3.4 exist, and must not call archTrapCatch() or ktestFail(). A level-triggered
 * source must be deasserted before the handler returns, or it fires again straight after the EOI.
 * The kernel sends the EOI itself, after the handler returns. */
#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include "uapi/status.h"

#include <stdint.h>

typedef void (*IrqHandler)(uint32_t vector, void *ctx);

#define IRQ_ACTIVE_LOW (1u << 0) /* irqRouteGsi flags: polarity (default active high) ... */
#define IRQ_LEVEL      (1u << 1) /* ... and trigger (default edge) */

/* Brings up the interrupt controllers: remaps and fully masks the 8259, initializes the local
 * APIC, maps and masks the IOAPICs from acpiGetInfo()'s MADT (no usable MADT means a local APIC
 * only: a warning is logged and every irqRoute* call returns STATUS_ERR_NOT_FOUND). Does NOT set
 * IF. Boot-time, BSP, IF=0, once (a second call panics). Panics only if the CPU has no usable
 * local APIC or its registers cannot be mapped. */
void irqInit(void);

/* Every Status-returning function below returns STATUS_ERR_INVALID before irqInit() and from IRQ
 * context (irqDepth() > 0). Locks: none named; the tables (irq.c) and then the IOAPIC index/data
 * pair (ioapic.c) are protected by IRQ-disable sections (archIrqSave), single CPU until M3.4, in
 * that order; vmmLock/pmmLock are never taken inside one. IRQ-safe with respect to IF (callable
 * with IF=0 or 1). May sleep: no. */

/* Allocates the lowest free dynamic vector in [48, 239]. STATUS_ERR_NO_MEMORY if none is left. */
Status irqAllocVector(uint32_t *outVector);
/* Frees an allocated vector. STATUS_ERR_INVALID if it is not allocated, still has a handler, or is
 * still routed from a GSI. */
Status irqFreeVector(uint32_t vector);
/* Registers `handler` for an allocated vector, or for one of the fixed ARCHITECTURE §7.2 vectors
 * (0xF0-0xF3 IPIs, 0xFE LAPIC timer). STATUS_ERR_INVALID: handler NULL, the vector neither
 * allocated nor fixed, or already registered. */
Status irqRegister(uint32_t vector, IrqHandler handler, void *ctx);
/* STATUS_ERR_INVALID if nothing is registered, or the vector is still routed from an unmasked
 * GSI (mask it first). */
Status irqUnregister(uint32_t vector);

/* Routes legacy ISA IRQ `isaIrq` (0-15) to `vector` through its ISO-resolved GSI, with the
 * polarity and trigger the MADT interrupt source override (or the ISA default, edge/active-high)
 * gives. The pin is left MASKED: irqUnmaskGsi() starts delivery. Fixed delivery, physical
 * destination, the boot CPU. STATUS_ERR_INVALID: isaIrq > 15, the vector is not allocated or is
 * already routed, or the GSI is. STATUS_ERR_NOT_FOUND: no MADT/IOAPIC, ISA IRQ taken over by an
 * override of another source (e.g. IRQ 2 when IRQ 0 is overridden to GSI 2), GSI not covered by
 * an IOAPIC, or reserved as a MADT NMI source. STATUS_ERR_UNSUPPORTED: the boot CPU's APIC ID
 * exceeds 254 (needs interrupt remapping). `*outGsi` (may be NULL) receives the GSI. */
Status irqRouteIsa(uint32_t isaIrq, uint32_t vector, uint32_t *outGsi);
/* Same for an explicit GSI and IRQ_ACTIVE_LOW/IRQ_LEVEL flags (PCI INTx later). Two pins never
 * share a vector: the LAPIC's EOI broadcast matches by vector, so sharing would break level
 * pins. */
Status irqRouteGsi(uint32_t gsi, uint32_t vector, uint32_t flags);
/* STATUS_ERR_NOT_FOUND if `gsi` is not routed; STATUS_ERR_INVALID if its vector has no handler. */
Status irqUnmaskGsi(uint32_t gsi);
Status irqMaskGsi(uint32_t gsi); /* STATUS_ERR_NOT_FOUND if `gsi` is not routed */
/* Masks the pin, waits (bounded) for a level pin's Remote IRR to clear, resets the entry and
 * forgets the route. STATUS_ERR_NOT_FOUND if `gsi` is not routed. Caveat: the wait runs with IRQs
 * off, so on one CPU a level pin that is still in service never clears and the full bound (about
 * a million IOAPIC reads) is spent before a warning; unroute level pins only once drained. Edge
 * pins are never polled. */
Status irqUnrouteGsi(uint32_t gsi);

/* 1 while an interrupt handler is running on this CPU, else 0 (BSP-global until M3.5 moves it to
 * the per-CPU area). No locks; IRQ-safe. */
uint32_t irqDepth(void);

typedef struct {
    uint64_t spurious;       /* vector 0xFF (no EOI is sent for these) */
    uint64_t legacySpurious; /* 8259 IRQ7/IRQ15 that were not real */
    uint64_t unhandled;      /* a vector with no registered handler (EOI'd and counted) */
} IrqStats;
/* An atomic snapshot (IRQ-disabled). */
void irqGetStats(IrqStats *out);
/* How many times `vector` has been dispatched to its handler path. No locks; IRQ-safe. */
uint64_t irqVectorCount(uint32_t vector);

#endif
