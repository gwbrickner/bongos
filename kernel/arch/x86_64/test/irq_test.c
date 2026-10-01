/* ktests for the interrupt controllers (M3.2, D-172..D-175, ROADMAP M3.2): the 8259 is remapped
 * and masked, the local APIC is in the state D-172 describes, vectors are allocated and dispatched
 * (self-IPI), the spurious/unhandled/legacy-spurious policies hold (D-173), and a real device
 * interrupt -- the PIT's channel 0 on ISA IRQ 0 -- reaches its handler through the IOAPIC (the 8259
 * and LINT0 are masked, so nothing else could deliver it).
 *
 * Handlers run with IF=0 and only record into volatile globals; the test asserts after the handler
 * returns (KTEST_ASSERT is not allowed in an IRQ handler). Waits poll with archPause() against an
 * uncalibrated TSC budget -- never `hlt`, which would hang if the interrupt landed between the
 * check and the halt. M3.3 replaces the TSC budget with timeMonotonicNs(). */
#include "apic.h"

#include "acpi.h"
#include "irq.h"
#include "ktest.h"

#include <arch/cpu.h>
#include <arch/io.h>
#include <stdbool.h>
#include <stdint.h>

#define IRQ_TEST_TSC_BUDGET (1ULL << 33)

typedef struct {
    volatile uint32_t count;
    volatile uint32_t lastVector;
    volatile uint32_t depthSeen;
    volatile uint32_t ifSeen;
} Seen;

static void recordHandler(uint32_t vector, void *ctx) {
    Seen *s = (Seen *)ctx;
    s->lastVector = vector;
    s->depthSeen = irqDepth();
    s->ifSeen = archInterruptsEnabled() ? 1 : 0;
    s->count++;
}

/* Polls until `*counter >= target` or the TSC budget runs out. */
static bool waitCount(volatile uint32_t *counter, uint32_t target) {
    uint64_t start = archReadTsc();
    while (*counter < target) {
        if (archReadTsc() - start > IRQ_TEST_TSC_BUDGET) {
            return false;
        }
        archPause();
    }
    return true;
}

static void spinPauses(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        archPause();
    }
}

static bool isrAllClear(void) {
    for (uint32_t i = 0; i < 8; i++) {
        if (lapicRead(LAPIC_REG_ISR + 0x10u * i) != 0) {
            return false;
        }
    }
    return true;
}

KTEST(irq_pic_masked) {
    KTEST_ASSERT_EQ(pic8259ReadImr(0), 0xFF);
    KTEST_ASSERT_EQ(pic8259ReadImr(1), 0xFF);
}

KTEST(irq_legacy_spurious_counted) {
    IrqStats before, after;
    irqGetStats(&before);
    __asm__ volatile("int $39"); /* a spurious IRQ7: the master's ISR bit 7 is clear */
    irqGetStats(&after);
    KTEST_ASSERT_EQ(after.legacySpurious, before.legacySpurious + 1);
    __asm__ volatile("int $47"); /* IRQ15: the slave's ISR bit 7 is clear (master EOI'd) */
    irqGetStats(&after);
    KTEST_ASSERT_EQ(after.legacySpurious, before.legacySpurious + 2);
    KTEST_ASSERT_EQ(irqDepth(), 0);
    KTEST_ASSERT(archInterruptsEnabled());
}

/* The LVT pin the MADT's LAPIC-NMI entry for the boot CPU names, or -1 for neither/none. */
static bool madtNmiOnLint(const AcpiMadtInfo *madt, uint32_t lint) {
    uint32_t id = lapicId();
    uint32_t uid = ACPI_LAPIC_NMI_ALL;
    for (uint32_t i = 0; i < madt->cpuCount; i++) {
        if (madt->cpus[i].apicId == id) {
            uid = madt->cpus[i].uid;
        }
    }
    for (uint32_t i = 0; i < madt->lapicNmiCount; i++) {
        const AcpiLapicNmi *n = &madt->lapicNmis[i];
        if (n->lint == lint && (n->uid == ACPI_LAPIC_NMI_ALL || n->uid == uid)) {
            return true;
        }
    }
    return false;
}

KTEST(irq_lapic_state) {
    KTEST_ASSERT_EQ(lapicRead(LAPIC_REG_SVR), LAPIC_SVR_VALUE);
    KTEST_ASSERT((lapicRead(LAPIC_REG_TIMER) & LAPIC_LVT_MASKED) != 0);
    KTEST_ASSERT((lapicRead(LAPIC_REG_ERROR) & LAPIC_LVT_MASKED) != 0);
    KTEST_ASSERT_EQ(lapicRead(LAPIC_REG_TPR), 0);
    KTEST_ASSERT(isrAllClear());
    KTEST_ASSERT_EQ(lapicId(), archCpuApicId());

    uint32_t r[4];
    archCpuid(1, 0, r);
    KTEST_ASSERT_EQ(lapicIsX2apic(), ((r[2] >> 21) & 1u) != 0); /* x2APIC whenever CPUID allows */

    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK);
    for (uint32_t lint = 0; lint < 2; lint++) {
        uint32_t v = lapicRead(lint == 0 ? LAPIC_REG_LINT0 : LAPIC_REG_LINT1);
        if (madtNmiOnLint(&a->madt, lint)) {
            KTEST_ASSERT((v & LAPIC_LVT_MASKED) == 0);
            KTEST_ASSERT_EQ(v & 0x700, LAPIC_LVT_NMI_MODE);
            KTEST_ASSERT_EQ(v & (1u << 15), 0); /* NMI is always edge */
        } else {
            KTEST_ASSERT((v & LAPIC_LVT_MASKED) != 0);
        }
    }
    KTEST_ASSERT_EQ(lapicReadEsr(), 0);
}

KTEST(irq_vector_alloc_exhaust) {
    static uint32_t got[256];
    uint32_t n = 0;
    uint32_t v;
    while (irqAllocVector(&v) == STATUS_OK) {
        KTEST_ASSERT(v >= 48 && v <= 239);
        KTEST_ASSERT(n < 256);
        got[n++] = v;
    }
    uint32_t total = n;
    for (uint32_t i = 0; i < n; i++) {
        KTEST_ASSERT(irqFreeVector(got[i]) == STATUS_OK);
    }
    KTEST_ASSERT_EQ(total, 192);
    KTEST_ASSERT(irqFreeVector(got[0]) == STATUS_ERR_INVALID); /* double free */
    KTEST_ASSERT(irqFreeVector(47) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqFreeVector(240) == STATUS_ERR_INVALID);
}

KTEST(irq_register_rules) {
    static Seen seen;
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, NULL, &seen) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqRegister(v + 1, recordHandler, &seen) == STATUS_ERR_INVALID); /* unallocated */
    KTEST_ASSERT(irqRegister(40, recordHandler, &seen) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqRegister(v, recordHandler, &seen) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, recordHandler, &seen) == STATUS_ERR_INVALID); /* already */
    KTEST_ASSERT(irqFreeVector(v) == STATUS_ERR_INVALID); /* still has a handler */
    KTEST_ASSERT(irqUnregister(v) == STATUS_OK);
    KTEST_ASSERT(irqUnregister(v) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}

KTEST(irq_self_ipi_delivered) {
    static Seen seen;
    seen.count = 0;
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, recordHandler, &seen) == STATUS_OK);
    KTEST_ASSERT(archInterruptsEnabled());
    uint64_t count0 = irqVectorCount(v); /* cumulative: earlier tests may have used v */

    lapicSendSelfIpi((uint8_t)v);
    KTEST_ASSERT(waitCount(&seen.count, 1));
    KTEST_ASSERT_EQ(seen.lastVector, v);
    KTEST_ASSERT_EQ(seen.depthSeen, 1);
    KTEST_ASSERT_EQ(seen.ifSeen, 0); /* the gate cleared IF */
    KTEST_ASSERT_EQ(irqDepth(), 0);
    KTEST_ASSERT(!lapicIsrBit((uint8_t)v)); /* the EOI retired it */
    KTEST_ASSERT_EQ(lapicReadEsr(), 0);

    lapicSendSelfIpi((uint8_t)v); /* a missing EOI would block this second delivery */
    KTEST_ASSERT(waitCount(&seen.count, 2));
    KTEST_ASSERT_EQ(irqVectorCount(v), count0 + 2);

    KTEST_ASSERT(irqUnregister(v) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}

KTEST(irq_self_ipi_pending_while_if0) {
    static Seen seen;
    seen.count = 0;
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, recordHandler, &seen) == STATUS_OK);

    uint64_t f = archIrqSave();
    lapicSendSelfIpi((uint8_t)v);
    spinPauses(100000);
    uint32_t during = seen.count;
    bool pending = lapicIrrBit((uint8_t)v);
    archIrqRestore(f);
    KTEST_ASSERT_EQ(during, 0); /* IF=0: nothing delivered */
    KTEST_ASSERT(pending);
    KTEST_ASSERT(waitCount(&seen.count, 1)); /* IF=1 again: delivered (one instruction later) */
    KTEST_ASSERT(!lapicIsrBit((uint8_t)v));

    KTEST_ASSERT(irqUnregister(v) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}

KTEST(irq_spurious_vector_no_eoi) {
    static Seen seen;
    seen.count = 0;
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, recordHandler, &seen) == STATUS_OK);
    IrqStats before, after;
    irqGetStats(&before);

    uint64_t f = archIrqSave();
    lapicSendSelfIpi((uint8_t)v);  /* sits in IRR while IF=0 */
    __asm__ volatile("int $0xFF"); /* a software-raised spurious vector, taken despite IF=0 */
    irqGetStats(&after);
    bool stillPending = lapicIrrBit((uint8_t)v);
    uint32_t during = seen.count;
    bool isrClear = isrAllClear(); /* an EOI here would have had no ISR bit to retire anyway */
    archIrqRestore(f);

    KTEST_ASSERT_EQ(after.spurious, before.spurious + 1);
    KTEST_ASSERT(stillPending);
    KTEST_ASSERT_EQ(during, 0);
    KTEST_ASSERT(isrClear);
    KTEST_ASSERT(waitCount(&seen.count, 1)); /* the real vector arrives exactly once */
    spinPauses(10000);
    KTEST_ASSERT_EQ(seen.count, 1);
    KTEST_ASSERT(isrAllClear());

    KTEST_ASSERT(irqUnregister(v) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}

KTEST(irq_unhandled_vector_eoi) {
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK); /* allocated but never registered */
    IrqStats before, after;
    irqGetStats(&before);

    lapicSendSelfIpi((uint8_t)v);
    uint64_t start = archReadTsc();
    for (;;) {
        irqGetStats(&after);
        if (after.unhandled >= before.unhandled + 1 ||
            archReadTsc() - start > IRQ_TEST_TSC_BUDGET) {
            break;
        }
        archPause();
    }
    KTEST_ASSERT_EQ(after.unhandled, before.unhandled + 1);
    KTEST_ASSERT(!lapicIsrBit((uint8_t)v)); /* EOI'd, not left in service */

    lapicSendSelfIpi((uint8_t)v); /* the second one is only counted (the log line is once) */
    start = archReadTsc();
    for (;;) {
        irqGetStats(&after);
        if (after.unhandled >= before.unhandled + 2 ||
            archReadTsc() - start > IRQ_TEST_TSC_BUDGET) {
            break;
        }
        archPause();
    }
    KTEST_ASSERT_EQ(after.unhandled, before.unhandled + 2);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}

KTEST(irq_ioapic_masked_at_init) {
    KTEST_ASSERT(ioapicCount() >= 1);
    for (uint32_t i = 0; i < ioapicCount(); i++) {
        uint32_t base, pins;
        KTEST_ASSERT(ioapicInfo(i, &base, &pins));
        KTEST_ASSERT(pins >= 1);
        for (uint32_t p = 0; p < pins; p++) {
            uint64_t rte;
            KTEST_ASSERT(ioapicReadRte(base + p, &rte) == STATUS_OK);
            KTEST_ASSERT((rte & (1ULL << 16)) != 0);
        }
    }
    uint64_t rte;
    uint32_t base, pins;
    KTEST_ASSERT(ioapicInfo(0, &base, &pins));
    KTEST_ASSERT(ioapicReadRte(base + pins, &rte) != STATUS_OK || ioapicCount() > 1);
}

/* The GSI and flags ISA IRQ `irq` should resolve to, computed here straight from the MADT
 * overrides (independently of irqCoreIsaRoute): the first override with source == irq, else
 * identity. */
static void expectedIsa(const AcpiMadtInfo *m, uint32_t irq, uint32_t *gsi, bool *low,
                        bool *level) {
    *gsi = irq;
    *low = false;
    *level = false;
    for (uint32_t i = 0; i < m->isoCount; i++) {
        if (m->isos[i].bus == 0 && m->isos[i].source == irq) {
            *gsi = m->isos[i].gsi;
            *low = (m->isos[i].flags & 3u) == 3u;
            *level = ((m->isos[i].flags >> 2) & 3u) == 3u;
            return;
        }
    }
}

KTEST(irq_isa_route_matches_madt) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK);
    static Seen seen;
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);

    uint32_t wantGsi;
    bool wantLow, wantLevel;
    expectedIsa(&a->madt, 0, &wantGsi, &wantLow, &wantLevel);

    uint32_t gsi = 0xFFFF;
    KTEST_ASSERT(irqRouteIsa(0, v, &gsi) == STATUS_OK);
    KTEST_ASSERT_EQ(gsi, wantGsi);
    uint64_t rte;
    KTEST_ASSERT(ioapicReadRte(gsi, &rte) == STATUS_OK);
    KTEST_ASSERT_EQ(rte & 0xFF, v);
    KTEST_ASSERT_EQ((rte >> 8) & 7, 0);  /* Fixed */
    KTEST_ASSERT_EQ((rte >> 11) & 1, 0); /* physical */
    KTEST_ASSERT_EQ((rte >> 13) & 1, wantLow ? 1 : 0);
    KTEST_ASSERT_EQ((rte >> 15) & 1, wantLevel ? 1 : 0);
    KTEST_ASSERT((rte & (1ULL << 16)) != 0); /* left masked */
    KTEST_ASSERT_EQ(rte >> 56, lapicId());

    uint32_t v2;
    KTEST_ASSERT(irqAllocVector(&v2) == STATUS_OK);
    KTEST_ASSERT(irqRouteGsi(gsi, v2, 0) == STATUS_ERR_INVALID); /* the GSI is routed already */
    KTEST_ASSERT(irqRouteGsi(gsi + 1, v, 0) ==
                 STATUS_ERR_INVALID); /* the vector is routed already */
    KTEST_ASSERT(irqRouteGsi(0xFFFF, v2, 0) == STATUS_ERR_NOT_FOUND); /* no IOAPIC owns it */
    KTEST_ASSERT(irqRouteIsa(16, v2, NULL) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqUnmaskGsi(gsi) == STATUS_ERR_INVALID); /* no handler yet */
    KTEST_ASSERT(irqUnmaskGsi(gsi + 1) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_ERR_INVALID); /* still routed */

    /* ISA IRQ 2 is shadowed when IRQ 0 is overridden to GSI 2 (QEMU does exactly that). */
    uint32_t g2;
    bool l2, e2;
    expectedIsa(&a->madt, 0, &g2, &l2, &e2);
    if (g2 == 2) {
        KTEST_ASSERT(irqRouteIsa(2, v2, NULL) == STATUS_ERR_NOT_FOUND);
    }

    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_OK);
    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(ioapicReadRte(gsi, &rte) == STATUS_OK);
    KTEST_ASSERT((rte & (1ULL << 16)) != 0);
    KTEST_ASSERT_EQ(rte & 0xFF, 0);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v2) == STATUS_OK);
    (void)seen;
}

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_MODE0 0x30  /* channel 0, lobyte/hibyte, mode 0 (interrupt on terminal count) */
#define PIT_COUNT 11932 /* about 10 ms at 1.193182 MHz */

static void pitArm(void) {
    ioOutByte(PIT_CMD, PIT_MODE0); /* OUT goes low and the count halts until it is written */
    ioOutByte(PIT_CH0, PIT_COUNT & 0xFF);
    ioOutByte(PIT_CH0, PIT_COUNT >> 8);
}

/* The PIT's OUT pin through the read-back command (latch status of channel 0). */
static bool pitOutHigh(void) {
    ioOutByte(PIT_CMD, 0xE2);
    return (ioInByte(PIT_CH0) & 0x80) != 0;
}

KTEST(irq_ioapic_pit_routed) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK);
    static Seen seen;
    seen.count = 0;
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, recordHandler, &seen) == STATUS_OK);
    uint32_t gsi;
    KTEST_ASSERT(irqRouteIsa(0, v, &gsi) == STATUS_OK); /* ISA IRQ 0 -> GSI 2 on QEMU */

    /* Mode 0 halts the firmware's periodic tick and yields exactly one edge, with no handler ack.
     * QEMU latches an edge that arrived while the pin was masked and delivers it on unmask, so at
     * most one stale interrupt may show up before the real one (c0 <= 1). */
    uint64_t f = archIrqSave();
    ioOutByte(PIT_CMD, PIT_MODE0);
    KTEST_ASSERT(irqUnmaskGsi(gsi) == STATUS_OK);
    archIrqRestore(f);
    spinPauses(10000);
    uint32_t c0 = seen.count;
    KTEST_ASSERT(c0 <= 1);

    pitArm();
    if (!waitCount(&seen.count, c0 + 1)) {
        ktestFail(ktestCtx, __FILE__, __LINE__,
                  pitOutHigh() ? "PIT reached terminal count but the IRQ was not delivered"
                               : "PIT never reached terminal count");
        return;
    }
    KTEST_ASSERT_EQ(seen.lastVector, v);
    KTEST_ASSERT(!lapicTmrBit((uint8_t)v)); /* edge-triggered */
    KTEST_ASSERT(!lapicIsrBit((uint8_t)v));
    KTEST_ASSERT_EQ(lapicReadEsr(), 0);
    spinPauses(10000);
    KTEST_ASSERT_EQ(seen.count, c0 + 1); /* exactly one edge */

    /* Masked negative: the PIT fires again but nothing is delivered. */
    KTEST_ASSERT(irqMaskGsi(gsi) == STATUS_OK);
    pitArm();
    uint64_t start = archReadTsc();
    while (!pitOutHigh()) {
        KTEST_ASSERT(archReadTsc() - start <= IRQ_TEST_TSC_BUDGET);
        archPause();
    }
    spinPauses(10000);
    KTEST_ASSERT_EQ(seen.count, c0 + 1);

    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_OK);
    KTEST_ASSERT(irqUnregister(v) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}
