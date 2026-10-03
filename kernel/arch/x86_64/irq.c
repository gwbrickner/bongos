/* Interrupt dispatch, vector ownership and IOAPIC routing policy (ARCHITECTURE §7.2/§7.3,
 * D-172..D-174). The rules a handler lives by are in kernel/include/irq.h; the dispatch policy
 * (EOI after the handler, 0xFF without an EOI, 8259 spurious IRQ7/15, log-and-EOI for an
 * unregistered vector) is D-173. One CPU until M3.5: the tables are protected by IRQ-disable. */
#include <irq.h>

#include "include/apic.h"
#include "include/irq-core.h"
#include "include/trap-impl.h"

#include "acpi.h"
#include "klog.h"
#include "panic.h"
#include "preempt.h"
#include "lockdep.h"
#include "spinlock.h"

#include <arch/cpu.h>
#include <arch/io.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NO_GSI 0xFFFFFFFFu

typedef struct {
    IrqHandler handler;
    void *ctx;
} IrqSlot;

static IrqSlot slots[IRQ_VECTOR_COUNT];
static uint64_t vectorCounts[IRQ_VECTOR_COUNT];
static uint32_t vectorGsi[IRQ_VECTOR_COUNT];  /* the GSI routed to this vector, or NO_GSI */
static bool vectorUnmasked[IRQ_VECTOR_COUNT]; /* the routed pin is currently unmasked */
static uint64_t warnedUnhandled[IRQ_VECTOR_COUNT / 64];
static IrqVectorMap vectorMap;
static IrqStats stats;

/* Protects slots[], vectorGsi[], vectorUnmasked[] and the vector map across CPUs (D-201); the
 * dispatch path reads slots[] lock-free (release/acquire on the handler pointer) and the counters
 * are relaxed atomics. Lock order: irqLock -> ioapicLock. */
static Spinlock irqLockObj = SPINLOCK_INIT("irq");
static bool inited = false;
static const AcpiMadtInfo *madtInfo = NULL;

/* The vectors ARCHITECTURE §7.2 fixes (never allocated, but registrable): the four IPIs and the
 * LAPIC timer. */
static bool vectorIsFixed(uint32_t v) {
    return (v >= 0xF0 && v <= 0xF3) || v == 0xFE;
}

uint32_t irqDepth(void) {
    return cpuLocal()->irqDepth;
}

void irqInit(void) {
    if (inited) {
        panic("irq: irqInit() called twice");
    }
    irqVectorMapInit(&vectorMap);
    for (uint32_t v = 0; v < IRQ_VECTOR_COUNT; v++) {
        vectorGsi[v] = NO_GSI;
    }

    const AcpiInfo *info = acpiGetInfo();
    if (info != NULL && info->madtStatus == STATUS_OK) {
        madtInfo = &info->madt;
    }

    pic8259RemapAndMask();
    /* Run whatever PCAT_COMPAT says: writes to an absent 8259 are harmless. With no MADT the
     * flag is unknown and assumed set. */
    klogWrite(KLOG_INFO, "irq", "8259 remapped to 0x20/0x28 and masked (pcat=%u)",
              madtInfo != NULL ? (unsigned)madtInfo->pcatCompat : 1u);

    lapicInit(madtInfo);
    ioapicInitAll(madtInfo);
    if (ioapicCount() == 0) {
        klogWrite(KLOG_WARN, "irq", "no IOAPIC (%s); legacy IRQ routing disabled",
                  madtInfo == NULL ? "MADT unavailable" : "none usable");
    } else {
        for (uint32_t irq = 0; irq < IRQ_ISA_COUNT; irq++) {
            IrqIsaRoute r;
            bool warn = false;
            (void)irqCoreIsaRoute(madtInfo->isos, madtInfo->isoCount, irq, &r, &warn);
            if (warn) {
                klogWrite(KLOG_WARN, "irq",
                          "ignored or odd interrupt source overrides in the MADT");
                break;
            }
        }
    }
    inited = true;
}

/* --- vector ownership and handlers --------------------------------------------------------- */

static bool callable(void) {
    return inited && cpuLocal()->irqDepth == 0;
}

Status irqAllocVector(uint32_t *outVector) {
    if (!callable() || outVector == NULL) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = spinLockIrqSave(&irqLockObj);
    Status st = irqVectorMapAlloc(&vectorMap, outVector);
    spinUnlockIrqRestore(&irqLockObj, f);
    return st;
}

Status irqFreeVector(uint32_t vector) {
    if (!callable()) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = spinLockIrqSave(&irqLockObj);
    Status st = STATUS_ERR_INVALID;
    if (vector < IRQ_VECTOR_COUNT && slots[vector].handler == NULL && vectorGsi[vector] == NO_GSI) {
        st = irqVectorMapFree(&vectorMap, vector);
    }
    spinUnlockIrqRestore(&irqLockObj, f);
    return st;
}

Status irqRegister(uint32_t vector, IrqHandler handler, void *ctx) {
    if (!callable() || handler == NULL || vector >= IRQ_VECTOR_COUNT) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = spinLockIrqSave(&irqLockObj);
    Status st = STATUS_ERR_INVALID;
    if ((irqVectorMapIsAllocated(&vectorMap, vector) || vectorIsFixed(vector)) &&
        slots[vector].handler == NULL) {
        /* ctx before handler, release: another CPU (M3.5) that sees the handler sees its ctx. */
        slots[vector].ctx = ctx;
        __atomic_store_n(&slots[vector].handler, handler, __ATOMIC_RELEASE);
        st = STATUS_OK;
    }
    spinUnlockIrqRestore(&irqLockObj, f);
    return st;
}

Status irqUnregister(uint32_t vector) {
    if (!callable() || vector >= IRQ_VECTOR_COUNT) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = spinLockIrqSave(&irqLockObj);
    Status st = STATUS_ERR_INVALID;
    if (slots[vector].handler != NULL && !(vectorGsi[vector] != NO_GSI && vectorUnmasked[vector])) {
        __atomic_store_n(&slots[vector].handler, NULL, __ATOMIC_RELEASE);
        slots[vector].ctx = NULL;
        st = STATUS_OK;
    }
    spinUnlockIrqRestore(&irqLockObj, f);
    return st;
}

/* --- IOAPIC routing ------------------------------------------------------------------------ */

/* The vector `gsi` is routed to, or NO_GSI (used as "none") -- caller holds the IRQ-disable. */
static uint32_t vectorOfGsi(uint32_t gsi) {
    for (uint32_t v = 0; v < IRQ_VECTOR_COUNT; v++) {
        if (vectorGsi[v] == gsi) {
            return v;
        }
    }
    return NO_GSI;
}

Status irqRouteGsi(uint32_t gsi, uint32_t vector, uint32_t flags) {
    if (!callable() || (flags & ~(IRQ_ACTIVE_LOW | IRQ_LEVEL)) != 0 || vector >= IRQ_VECTOR_COUNT) {
        return STATUS_ERR_INVALID;
    }
    uint32_t apicId = cpuLocalOf(0)->apicId; /* device IRQs stay on the boot CPU (D-201) */
    uint64_t f = spinLockIrqSave(&irqLockObj);
    Status st;
    if (!ioapicGsiUsable(gsi)) {
        st = STATUS_ERR_NOT_FOUND;
    } else if (!irqVectorMapIsAllocated(&vectorMap, vector) || vectorGsi[vector] != NO_GSI ||
               vectorOfGsi(gsi) != NO_GSI) {
        st = STATUS_ERR_INVALID;
    } else if (apicId > 254) {
        st = STATUS_ERR_UNSUPPORTED; /* 0xFF is broadcast; larger IDs need interrupt remapping */
    } else {
        uint64_t rte = irqCoreRteEncode((uint8_t)vector, (flags & IRQ_ACTIVE_LOW) != 0,
                                        (flags & IRQ_LEVEL) != 0, true, (uint8_t)apicId);
        st = ioapicWriteRte(gsi, rte);
        if (st == STATUS_OK) {
            vectorGsi[vector] = gsi;
            vectorUnmasked[vector] = false;
        }
    }
    spinUnlockIrqRestore(&irqLockObj, f);
    return st;
}

Status irqRouteIsa(uint32_t isaIrq, uint32_t vector, uint32_t *outGsi) {
    if (!callable()) {
        return STATUS_ERR_INVALID;
    }
    if (isaIrq >= IRQ_ISA_COUNT) {
        return STATUS_ERR_INVALID;
    }
    if (madtInfo == NULL || ioapicCount() == 0) {
        return STATUS_ERR_NOT_FOUND;
    }
    IrqIsaRoute r;
    Status st = irqCoreIsaRoute(madtInfo->isos, madtInfo->isoCount, isaIrq, &r, NULL);
    if (st != STATUS_OK) {
        return st;
    }
    st = irqRouteGsi(r.gsi, vector, (r.activeLow ? IRQ_ACTIVE_LOW : 0) | (r.level ? IRQ_LEVEL : 0));
    if (st == STATUS_OK && outGsi != NULL) {
        *outGsi = r.gsi;
    }
    return st;
}

static Status setGsiMask(uint32_t gsi, bool masked) {
    if (!callable()) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = spinLockIrqSave(&irqLockObj);
    uint32_t v = vectorOfGsi(gsi);
    Status st;
    if (v == NO_GSI) {
        st = STATUS_ERR_NOT_FOUND;
    } else if (!masked && slots[v].handler == NULL) {
        st = STATUS_ERR_INVALID;
    } else {
        st = ioapicSetMask(gsi, masked);
        if (st == STATUS_OK) {
            vectorUnmasked[v] = !masked;
        }
    }
    spinUnlockIrqRestore(&irqLockObj, f);
    return st;
}

Status irqUnmaskGsi(uint32_t gsi) {
    return setGsiMask(gsi, false);
}

Status irqMaskGsi(uint32_t gsi) {
    return setGsiMask(gsi, true);
}

Status irqUnrouteGsi(uint32_t gsi) {
    if (!callable()) {
        return STATUS_ERR_INVALID;
    }
    uint64_t f = spinLockIrqSave(&irqLockObj);
    uint32_t v = vectorOfGsi(gsi);
    Status st;
    if (v == NO_GSI) {
        st = STATUS_ERR_NOT_FOUND;
    } else {
        st = ioapicSetMask(gsi, true);
        if (st == STATUS_OK) {
            /* A level pin whose Remote IRR is still set must not be reprogrammed (82093AA §3.2.4);
             * the EOI that clears it arrives only once its in-service vector retires. */
            uint64_t rte = 0;
            /* Remote IRR is undefined for an edge entry, so only a level pin is polled. IRQs are
             * off here, so on one CPU the EOI that clears it cannot run: the bound is a stall
             * (D-174, sweep lead S4); the first level-pin user must unroute only drained pins. */
            if (ioapicReadRte(gsi, &rte) == STATUS_OK && (rte & (1ULL << 15)) != 0) {
                for (uint32_t i = 0; i < 1000000; i++) {
                    if (ioapicReadRte(gsi, &rte) != STATUS_OK || (rte & (1ULL << 14)) == 0) {
                        break;
                    }
                    archPause();
                }
                if (rte & (1ULL << 14)) {
                    klogWrite(KLOG_WARN, "irq", "GSI %u: Remote IRR still set at unroute", gsi);
                }
            }
            st = ioapicWriteRte(gsi, irqCoreRteEncode(0, false, false, true, 0));
            vectorGsi[v] = NO_GSI;
            vectorUnmasked[v] = false;
        }
    }
    spinUnlockIrqRestore(&irqLockObj, f);
    return st;
}

void irqGetStats(IrqStats *out) {
    out->spurious = __atomic_load_n(&stats.spurious, __ATOMIC_RELAXED);
    out->legacySpurious = __atomic_load_n(&stats.legacySpurious, __ATOMIC_RELAXED);
    out->unhandled = __atomic_load_n(&stats.unhandled, __ATOMIC_RELAXED);
}

uint64_t irqVectorCount(uint32_t vector) {
    return vector < IRQ_VECTOR_COUNT ? __atomic_load_n(&vectorCounts[vector], __ATOMIC_RELAXED) : 0;
}

/* --- dispatch ------------------------------------------------------------------------------ */

void irqDispatch(TrapFrame *f) {
    uint32_t v = (uint32_t)f->vector;
    if (!inited) {
        panic("irq: vector %u before irqInit", v);
    }

    if (v == LAPIC_SPURIOUS_VECTOR) {
        /* A spurious interrupt never sets an ISR bit, so an EOI here would retire some other
         * in-service vector instead (SDM Vol 3A §10.9). */
        __atomic_fetch_add(&stats.spurious, 1, __ATOMIC_RELAXED);
        return;
    }

    if (v >= 0x20 && v <= 0x2F) {
        /* The 8259 and LINT0 are masked, so only a spurious IRQ7/IRQ15 or a software `int` can
         * arrive. A real IRQ7/15 has its ISR bit set; a spurious one does not. The LAPIC ISR is not
         * involved (ExtINT never uses it), so no LAPIC EOI either way. */
        if (v == 0x27) {
            if ((pic8259ReadIsr(0) & 0x80) == 0) {
                __atomic_fetch_add(&stats.legacySpurious, 1, __ATOMIC_RELAXED);
                return;
            }
        } else if (v == 0x2F) {
            if ((pic8259ReadIsr(1) & 0x80) == 0) {
                __atomic_fetch_add(&stats.legacySpurious, 1, __ATOMIC_RELAXED);
                pic8259EoiMaster(); /* the cascade line (IRQ2) was real */
                return;
            }
        }
        panic("irq: 8259 vector %u delivered while fully masked", v);
    }

    if (cpuLocal()->irqDepth != 0) {
        panic("irq: nested interrupt (vector %u inside a handler)", v);
    }
    cpuLocal()->irqDepth = 1;
    __atomic_fetch_add(&vectorCounts[v], 1, __ATOMIC_RELAXED);
    IrqHandler h = __atomic_load_n(&slots[v].handler, __ATOMIC_ACQUIRE);
    if (h != NULL) {
        uint32_t preemptBefore = preemptCount();
        uint32_t heldBefore = lockdepHeldDepth();
        h(v, slots[v].ctx);
        /* D-187: a handler must return with every lock it took released and preemption balanced. */
        if (preemptCount() != preemptBefore || lockdepHeldDepth() != heldBefore) {
            panic("irq: handler for vector %u returned holding a spinlock or with preemption "
                  "disabled",
                  v);
        }
    } else {
        /* Firmware leaves stale IRR bits, and QEMU latches edges on masked pins and delivers them
         * on unmask: neither may kill boot. Count it, log the first one per vector, and EOI. */
        __atomic_fetch_add(&stats.unhandled, 1, __ATOMIC_RELAXED);
        uint64_t bit = 1ULL << (v % 64);
        if ((__atomic_fetch_or(&warnedUnhandled[v / 64], bit, __ATOMIC_RELAXED) & bit) == 0) {
            klogWrite(KLOG_WARN, "irq",
                      "unhandled vector %u (EOI sent; further occurrences counted silently)", v);
        }
    }
    cpuLocal()->irqDepth = 0;
    if (v >= 48 && v <= 254) {
        lapicEoi();
    }
}
