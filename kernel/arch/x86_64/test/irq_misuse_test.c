/* Adversarial ktests for the M3.2 interrupt code (bug-sweeper, docs/sweeps/M3.2.md): the irq.h API
 * refuses every call from IRQ context (D-173) and every misuse of vectors and routes (D-174); the
 * EOI is sent after the handler and never for the spurious paths (D-173), proven from *inside* a
 * handler where an in-service bit exists to be wrongly retired; the fixed ARCHITECTURE §7.2 vectors
 * dispatch; every ISA IRQ resolves to the GSI, polarity and trigger the MADT gives (not only IRQ
 * 0); archTrapCatch refuses to longjmp out of a handler and restores IF after a caught fault.
 *
 * Handlers only record into volatile globals; the asserts run after they return. */
#include "apic.h"

#include "acpi.h"
#include "irq.h"
#include "ktest.h"

#include <arch/cpu.h>
#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

#define MISUSE_TSC_BUDGET (1ULL << 33)

static bool waitFor(volatile uint32_t *counter, uint32_t target) {
    uint64_t start = archReadTsc();
    while (*counter < target) {
        if (archReadTsc() - start > MISUSE_TSC_BUDGET) {
            return false;
        }
        archPause();
    }
    return true;
}

static bool lapicIsrEmpty(void) {
    for (uint32_t i = 0; i < 8; i++) {
        if (lapicRead(LAPIC_REG_ISR + 0x10u * i) != 0) {
            return false;
        }
    }
    return true;
}

/* --- the API from inside a handler ---------------------------------------------------------- */

typedef struct {
    volatile uint32_t count;
    volatile uint32_t depth;
    volatile int32_t st[10];
    volatile uint32_t allocOut;
    volatile bool isrAtEntry, isrAfterSpurious, isrAfterLegacy;
    volatile uint64_t spuriousDelta, legacyDelta;
} InHandler;

static InHandler inHandler;

static void apiFromHandler(uint32_t vector, void *ctx) {
    InHandler *h = (InHandler *)ctx;
    h->depth = irqDepth();
    h->isrAtEntry = lapicIsrBit((uint8_t)vector);

    uint32_t out = 0xA5A5A5A5u;
    h->st[0] = irqAllocVector(&out);
    h->allocOut = out;
    h->st[1] = irqFreeVector(vector);
    h->st[2] = irqRegister(vector, apiFromHandler, ctx);
    h->st[3] = irqUnregister(vector);
    h->st[4] = irqRouteGsi(2, vector, 0);
    h->st[5] = irqRouteIsa(0, vector, NULL);
    h->st[6] = irqUnmaskGsi(2);
    h->st[7] = irqMaskGsi(2);
    h->st[8] = irqUnrouteGsi(2);
    h->st[9] = irqRegister(0xF0, apiFromHandler, ctx);

    /* The spurious paths must not EOI: this handler's own vector is the one in service, so an
     * EOI from either would retire it right here. */
    IrqStats a, b;
    irqGetStats(&a);
    __asm__ volatile("int $0xFF");
    h->isrAfterSpurious = lapicIsrBit((uint8_t)vector);
    __asm__ volatile("int $39");
    h->isrAfterLegacy = lapicIsrBit((uint8_t)vector);
    irqGetStats(&b);
    h->spuriousDelta = b.spurious - a.spurious;
    h->legacyDelta = b.legacySpurious - a.legacySpurious;
    h->count++;
}

KTEST(irq_api_refused_in_handler) {
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, apiFromHandler, &inHandler) == STATUS_OK);
    uint32_t allocatedBefore = 0;
    {
        /* How many vectors are free now, so a leak from inside the handler shows. */
        uint32_t got[192];
        uint32_t x;
        while (allocatedBefore < 192 && irqAllocVector(&x) == STATUS_OK) {
            got[allocatedBefore++] = x;
        }
        for (uint32_t i = 0; i < allocatedBefore; i++) {
            KTEST_ASSERT(irqFreeVector(got[i]) == STATUS_OK);
        }
    }

    lapicSendSelfIpi((uint8_t)v);
    KTEST_ASSERT(waitFor(&inHandler.count, 1));
    KTEST_ASSERT_EQ(inHandler.depth, 1);
    KTEST_ASSERT(inHandler.isrAtEntry); /* the EOI comes after the handler, not before */
    for (uint32_t i = 0; i < 10; i++) {
        KTEST_ASSERT_EQ(inHandler.st[i] == STATUS_ERR_INVALID ? 0xFFu : i, 0xFFu);
    }
    KTEST_ASSERT_EQ(inHandler.allocOut, 0xA5A5A5A5u);
    KTEST_ASSERT(inHandler.isrAfterSpurious); /* vector 0xFF sent no EOI */
    KTEST_ASSERT(inHandler.isrAfterLegacy);   /* a spurious IRQ7 sent no LAPIC EOI */
    KTEST_ASSERT_EQ(inHandler.spuriousDelta, 1);
    KTEST_ASSERT_EQ(inHandler.legacyDelta, 1);
    KTEST_ASSERT(!lapicIsrBit((uint8_t)v));
    KTEST_ASSERT(lapicIsrEmpty());
    KTEST_ASSERT_EQ(irqDepth(), 0);

    /* Nothing the handler tried took effect: v is still registered and allocated, 0xF0 is free,
     * GSI 2 is not routed, and no vector leaked. */
    KTEST_ASSERT(irqRegister(v, apiFromHandler, &inHandler) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqMaskGsi(2) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqUnregister(0xF0) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqUnregister(v) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
    uint32_t got[193];
    uint32_t n = 0;
    uint32_t x;
    while (n < 193 && irqAllocVector(&x) == STATUS_OK) {
        got[n++] = x;
    }
    for (uint32_t i = 0; i < n; i++) {
        KTEST_ASSERT(irqFreeVector(got[i]) == STATUS_OK);
    }
    KTEST_ASSERT_EQ(n, allocatedBefore + 1);
}

/* --- a handler re-raising its own vector ----------------------------------------------------- */

typedef struct {
    volatile uint32_t count;
} Rearm;

static void rearmHandler(uint32_t vector, void *ctx) {
    Rearm *r = (Rearm *)ctx;
    if (r->count++ == 0) {
        lapicSendSelfIpi((uint8_t)vector); /* IRR while in service: delivered once after the EOI */
    }
}

KTEST(irq_self_ipi_from_handler_redelivered) {
    static Rearm r;
    r.count = 0;
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v, rearmHandler, &r) == STATUS_OK);
    lapicSendSelfIpi((uint8_t)v);
    KTEST_ASSERT(waitFor(&r.count, 2));
    for (uint32_t i = 0; i < 100000; i++) {
        archPause();
    }
    KTEST_ASSERT_EQ(r.count, 2);
    KTEST_ASSERT(lapicIsrEmpty());
    KTEST_ASSERT(irqUnregister(v) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}

/* --- two pending vectors: higher priority class first, each exactly once ---------------------- */

typedef struct {
    volatile uint32_t count;
    volatile uint32_t order[4];
} Order;

static void orderHandler(uint32_t vector, void *ctx) {
    Order *o = (Order *)ctx;
    if (o->count < 4) {
        o->order[o->count] = vector;
    }
    o->count++;
}

KTEST(irq_two_pending_vectors_priority_order) {
    static Order o;
    o.count = 0;
    uint32_t got[32];
    uint32_t n = 0;
    while (n < 32 && irqAllocVector(&got[n]) == STATUS_OK) {
        n++;
    }
    KTEST_ASSERT(n == 32);
    uint32_t lo = got[0], hi = got[31]; /* 32 apart at least: different priority classes */
    KTEST_ASSERT(hi / 16 > lo / 16);
    for (uint32_t i = 1; i < 31; i++) {
        KTEST_ASSERT(irqFreeVector(got[i]) == STATUS_OK);
    }
    KTEST_ASSERT(irqRegister(lo, orderHandler, &o) == STATUS_OK);
    KTEST_ASSERT(irqRegister(hi, orderHandler, &o) == STATUS_OK);

    uint64_t f = archIrqSave();
    lapicSendSelfIpi((uint8_t)lo);
    lapicSendSelfIpi((uint8_t)hi);
    archIrqRestore(f);
    KTEST_ASSERT(waitFor(&o.count, 2));
    for (uint32_t i = 0; i < 100000; i++) {
        archPause();
    }
    KTEST_ASSERT_EQ(o.count, 2);
    KTEST_ASSERT_EQ(o.order[0], hi);
    KTEST_ASSERT_EQ(o.order[1], lo);
    KTEST_ASSERT(lapicIsrEmpty());
    KTEST_ASSERT(irqUnregister(lo) == STATUS_OK);
    KTEST_ASSERT(irqUnregister(hi) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(lo) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(hi) == STATUS_OK);
}

/* --- fixed vectors and out-of-range vector numbers ------------------------------------------ */

typedef struct {
    volatile uint32_t count;
    volatile uint32_t lastVector;
} Fixed;

static void fixedHandler(uint32_t vector, void *ctx) {
    Fixed *s = (Fixed *)ctx;
    s->lastVector = vector;
    s->count++;
}

KTEST(irq_fixed_vectors_dispatch) {
    static Fixed s;
    static const uint32_t fixed[] = {0xF0, 0xF1, 0xF2, 0xF3, 0xFE};
    s.count = 0;
    for (uint32_t i = 0; i < 5; i++) {
        KTEST_ASSERT(irqRegister(fixed[i], fixedHandler, &s) == STATUS_OK);
        KTEST_ASSERT(irqRegister(fixed[i], fixedHandler, &s) == STATUS_ERR_INVALID);
        KTEST_ASSERT(irqFreeVector(fixed[i]) == STATUS_ERR_INVALID); /* never allocated */
    }
    uint64_t c0 = irqVectorCount(0xF0);
    __asm__ volatile("int $0xF0");
    KTEST_ASSERT_EQ(s.lastVector, 0xF0);
    __asm__ volatile("int $0xF1");
    KTEST_ASSERT_EQ(s.lastVector, 0xF1);
    __asm__ volatile("int $0xF2");
    KTEST_ASSERT_EQ(s.lastVector, 0xF2);
    __asm__ volatile("int $0xF3");
    KTEST_ASSERT_EQ(s.lastVector, 0xF3);
    __asm__ volatile("int $0xFE");
    KTEST_ASSERT_EQ(s.lastVector, 0xFE);
    KTEST_ASSERT_EQ(s.count, 5);
    KTEST_ASSERT_EQ(irqVectorCount(0xF0), c0 + 1);

    /* A real LAPIC delivery of a fixed vector is EOI'd like any other. */
    lapicSendSelfIpi(0xF1);
    KTEST_ASSERT(waitFor(&s.count, 6));
    KTEST_ASSERT_EQ(s.lastVector, 0xF1);
    KTEST_ASSERT(lapicIsrEmpty());

    for (uint32_t i = 0; i < 5; i++) {
        KTEST_ASSERT(irqUnregister(fixed[i]) == STATUS_OK);
        KTEST_ASSERT(irqUnregister(fixed[i]) == STATUS_ERR_INVALID);
    }

    /* Neither fixed nor allocatable, or not a vector at all. */
    static const uint32_t bad[] = {0, 31, 32, 47, 0xF4, 0xFD, 0xFF, 256, 0x10030, 0xFFFFFFFFu};
    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        KTEST_ASSERT(irqRegister(bad[i], fixedHandler, &s) == STATUS_ERR_INVALID);
        KTEST_ASSERT(irqUnregister(bad[i]) == STATUS_ERR_INVALID);
        KTEST_ASSERT(irqFreeVector(bad[i]) == STATUS_ERR_INVALID);
        KTEST_ASSERT(irqRouteGsi(2, bad[i], 0) == STATUS_ERR_INVALID);
    }
    KTEST_ASSERT_EQ(irqVectorCount(256), 0);
    KTEST_ASSERT_EQ(irqVectorCount(0xFFFFFFFFu), 0);
    KTEST_ASSERT(irqAllocVector(NULL) == STATUS_ERR_INVALID);
}

/* --- routing misuse -------------------------------------------------------------------------- */

static void routeHandler(uint32_t vector, void *ctx) {
    (void)vector;
    ((Fixed *)ctx)->count++;
}

KTEST(irq_route_misuse) {
    static Fixed s;
    s.count = 0;
    uint32_t base, pins;
    KTEST_ASSERT(ioapicInfo(0, &base, &pins));
    KTEST_ASSERT(pins >= 3);
    uint32_t gsi = base + pins - 1; /* the last pin: a PCI INTx line on q35, idle in CI */
    KTEST_ASSERT(ioapicGsiUsable(gsi));

    uint32_t v1, v2;
    KTEST_ASSERT(irqAllocVector(&v1) == STATUS_OK);
    KTEST_ASSERT(irqAllocVector(&v2) == STATUS_OK);
    KTEST_ASSERT(irqRegister(v1, routeHandler, &s) == STATUS_OK);

    /* Argument checks. */
    KTEST_ASSERT(irqRouteGsi(gsi, v1, 1u << 2) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqRouteGsi(gsi, v1, 0xFFFFFFFFu) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqRouteGsi(gsi, 0xF0, 0) == STATUS_ERR_INVALID);   /* fixed: never routable */
    KTEST_ASSERT(irqRouteGsi(gsi, v2 + 1, 0) == STATUS_ERR_INVALID); /* not allocated */
    KTEST_ASSERT(irqRouteGsi(0xFFFFFFFFu, v1, 0) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqRouteGsi(0xFFFFFFFEu, v1, 0) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqRouteGsi(base + pins, v1, 0) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqRouteIsa(0xFFFFFFFFu, v1, NULL) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqRouteIsa(16, v1, NULL) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqUnmaskGsi(gsi) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqMaskGsi(gsi) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqUnrouteGsi(0xFFFFFFFFu) == STATUS_ERR_NOT_FOUND);

    /* Level + active low is encoded, masked. */
    KTEST_ASSERT(irqRouteGsi(gsi, v1, IRQ_LEVEL | IRQ_ACTIVE_LOW) == STATUS_OK);
    uint64_t rte;
    KTEST_ASSERT(ioapicReadRte(gsi, &rte) == STATUS_OK);
    KTEST_ASSERT_EQ(rte & 0xFF, v1);
    KTEST_ASSERT_EQ((rte >> 13) & 1, 1);
    KTEST_ASSERT_EQ((rte >> 15) & 1, 1);
    KTEST_ASSERT_EQ((rte >> 16) & 1, 1);
    KTEST_ASSERT(irqRouteGsi(gsi, v1, 0) == STATUS_ERR_INVALID);     /* double route */
    KTEST_ASSERT(irqRouteGsi(gsi, v2, 0) == STATUS_ERR_INVALID);     /* GSI taken */
    KTEST_ASSERT(irqRouteGsi(gsi - 1, v1, 0) == STATUS_ERR_INVALID); /* vector taken */
    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_OK);

    /* Edge, active high: unmasked, the vector can be neither unregistered nor freed. */
    KTEST_ASSERT(irqRouteGsi(gsi, v1, 0) == STATUS_OK);
    KTEST_ASSERT(irqUnmaskGsi(gsi) == STATUS_OK);
    KTEST_ASSERT(ioapicReadRte(gsi, &rte) == STATUS_OK);
    KTEST_ASSERT_EQ((rte >> 16) & 1, 0);
    KTEST_ASSERT_EQ((rte >> 13) & 1, 0);
    KTEST_ASSERT_EQ((rte >> 15) & 1, 0);
    KTEST_ASSERT(irqUnmaskGsi(gsi) == STATUS_OK); /* idempotent */
    KTEST_ASSERT(irqUnregister(v1) == STATUS_ERR_INVALID);
    KTEST_ASSERT(irqFreeVector(v1) == STATUS_ERR_INVALID);

    /* Masked: unregister is allowed, a handler-less unmask is not, free still is not (routed). */
    KTEST_ASSERT(irqMaskGsi(gsi) == STATUS_OK);
    KTEST_ASSERT(irqMaskGsi(gsi) == STATUS_OK);
    KTEST_ASSERT(irqUnregister(v1) == STATUS_OK);
    KTEST_ASSERT(irqUnmaskGsi(gsi) == STATUS_ERR_INVALID);
    KTEST_ASSERT(ioapicReadRte(gsi, &rte) == STATUS_OK);
    KTEST_ASSERT_EQ((rte >> 16) & 1, 1);
    KTEST_ASSERT(irqFreeVector(v1) == STATUS_ERR_INVALID);

    /* Unroute frees both sides: the GSI takes another vector, the vector another GSI. */
    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_OK);
    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT(irqRouteGsi(gsi, v2, 0) == STATUS_OK);
    KTEST_ASSERT(irqRouteGsi(gsi - 1, v1, 0) == STATUS_OK);
    KTEST_ASSERT(ioapicReadRte(gsi, &rte) == STATUS_OK);
    KTEST_ASSERT_EQ(rte & 0xFF, v2);
    KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_OK);
    KTEST_ASSERT(irqUnrouteGsi(gsi - 1) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v1) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(v2) == STATUS_OK);
    KTEST_ASSERT_EQ(s.count, 0); /* an idle line never fired */

    /* MADT NMI sources are never routable. */
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK);
    for (uint32_t i = 0; i < a->madt.nmiSourceCount; i++) {
        KTEST_ASSERT(!ioapicGsiUsable(a->madt.nmiSources[i].gsi));
    }
}

/* --- every ISA IRQ against the MADT ---------------------------------------------------------- */

/* The expected route of ISA IRQ `irq` under QEMU's clean MADT (no duplicate or foreign-bus
 * overrides, which the host tests cover): the override for `irq`, else identity unless an override
 * of another source claimed GSI `irq`. Returns false for that "shadowed" case. */
static bool expectIsa(const AcpiMadtInfo *m, uint32_t irq, uint32_t *gsi, bool *low, bool *lvl) {
    *gsi = irq;
    *low = false;
    *lvl = false;
    for (uint32_t i = 0; i < m->isoCount; i++) {
        if (m->isos[i].bus == 0 && m->isos[i].source == irq) {
            *gsi = m->isos[i].gsi;
            *low = (m->isos[i].flags & 3u) == 3u;
            *lvl = ((m->isos[i].flags >> 2) & 3u) == 3u;
            return true;
        }
    }
    for (uint32_t i = 0; i < m->isoCount; i++) {
        if (m->isos[i].bus == 0 && m->isos[i].gsi == irq) {
            return false;
        }
    }
    return true;
}

KTEST(irq_isa_route_all_match_madt) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL && a->madtStatus == STATUS_OK);
    uint32_t v;
    KTEST_ASSERT(irqAllocVector(&v) == STATUS_OK);
    uint32_t levels = 0;
    for (uint32_t irq = 0; irq < 16; irq++) {
        uint32_t wantGsi;
        bool wantLow, wantLevel;
        bool routable = expectIsa(&a->madt, irq, &wantGsi, &wantLow, &wantLevel);
        uint32_t gsi = 0xDEAD;
        Status st = irqRouteIsa(irq, v, &gsi);
        if (!routable) {
            KTEST_ASSERT_EQ(st == STATUS_ERR_NOT_FOUND ? 0xFFu : irq, 0xFFu);
            KTEST_ASSERT_EQ(gsi, 0xDEAD);
            continue;
        }
        KTEST_ASSERT_EQ(st == STATUS_OK ? 0xFFu : irq, 0xFFu);
        KTEST_ASSERT_EQ(gsi, wantGsi);
        uint64_t rte;
        KTEST_ASSERT(ioapicReadRte(gsi, &rte) == STATUS_OK);
        KTEST_ASSERT_EQ(rte & 0xFF, v);
        KTEST_ASSERT_EQ((rte >> 8) & 7, 0);
        KTEST_ASSERT_EQ((rte >> 11) & 1, 0);
        KTEST_ASSERT_EQ(((rte >> 13) & 1) | (irq << 8), (wantLow ? 1u : 0u) | (irq << 8));
        KTEST_ASSERT_EQ(((rte >> 15) & 1) | (irq << 8), (wantLevel ? 1u : 0u) | (irq << 8));
        KTEST_ASSERT_EQ((rte >> 16) & 1, 1);
        KTEST_ASSERT_EQ(rte >> 56, lapicId());
        levels += wantLevel ? 1 : 0;
        KTEST_ASSERT(irqUnrouteGsi(gsi) == STATUS_OK);
    }
    /* QEMU's MADT overrides IRQ 9 (SCI) and the PCI-capable IRQs to level, so the level path is
     * actually covered here, not only the edge default. */
    KTEST_ASSERT(levels >= 1);
    KTEST_ASSERT(irqFreeVector(v) == STATUS_OK);
}

/* --- archTrapCatch and interrupt context ---------------------------------------------------- */

typedef struct {
    uint32_t vector;
    volatile uint32_t count;
    volatile bool softwareCaught;
    volatile uint32_t depthAfter;
} CatchCtx;

static CatchCtx catchCtx;

static void catchHandler(uint32_t vector, void *ctx) {
    (void)vector;
    CatchCtx *c = (CatchCtx *)ctx;
    /* Armed by the ktest below and matching the kind: only the IRQ-context rule refuses. Were it
     * honored, this would longjmp out of the handler, skipping the depth reset and the EOI. */
    c->softwareCaught = archTrapCatchSoftware(TRAP_CATCH_KERNEL_BUG, 0);
    c->count++;
}

static void catchArmedBody(void *arg) {
    CatchCtx *c = (CatchCtx *)arg;
    lapicSendSelfIpi((uint8_t)c->vector);
    (void)waitFor(&c->count, 1);
    c->depthAfter = irqDepth();
    /* The catch is still armed after the handler's refused attempt: this one resumes. */
    (void)archTrapCatchSoftware(TRAP_CATCH_KERNEL_BUG, 0);
}

KTEST(irq_trap_catch_refused_in_handler) {
    catchCtx.count = 0;
    catchCtx.softwareCaught = true;
    catchCtx.depthAfter = 0xFFu;
    KTEST_ASSERT(irqAllocVector(&catchCtx.vector) == STATUS_OK);
    KTEST_ASSERT(irqRegister(catchCtx.vector, catchHandler, &catchCtx) == STATUS_OK);
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, catchArmedBody, &catchCtx, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(catchCtx.count, 1);
    KTEST_ASSERT(!catchCtx.softwareCaught);
    KTEST_ASSERT_EQ(catchCtx.depthAfter, 0);
    KTEST_ASSERT_EQ(irqDepth(), 0);
    KTEST_ASSERT(lapicIsrEmpty());
    KTEST_ASSERT(archInterruptsEnabled());
    KTEST_ASSERT(irqUnregister(catchCtx.vector) == STATUS_OK);
    KTEST_ASSERT(irqFreeVector(catchCtx.vector) == STATUS_OK);
}

static void udInsideIrqSave(void *arg) {
    (void)arg;
    (void)archIrqSave(); /* never restored: the fault's longjmp skips it */
    __asm__ volatile("ud2");
}

KTEST(trap_catch_restores_if) {
    KTEST_ASSERT(archInterruptsEnabled());
    TrapCatchInfo info;
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_VEC(6), udInsideIrqSave, NULL, &info));
    KTEST_ASSERT_EQ(info.vector, 6);
    KTEST_ASSERT(archInterruptsEnabled());

    /* A catch armed with IF=0 leaves IF=0. */
    uint64_t f = archIrqSave();
    bool caught = archTrapCatch(TRAP_CATCH_VEC(6), udInsideIrqSave, NULL, &info);
    bool ifAfter = archInterruptsEnabled();
    archIrqRestore(f);
    KTEST_ASSERT(caught);
    KTEST_ASSERT(!ifAfter);
}
