/* x86 SMP bring-up (ARCHITECTURE §7.4, D-190..D-194, ROADMAP M3.5): the AP startup trampoline, the
 * INIT-SIPI-SIPI sequence, and the AP's own early init (apMain) up to its idle loop. The BSP starts
 * the APs one at a time; everything an AP needs (CpuLocal, stacks, per-CPU pmm/slab blobs) is
 * allocated by the BSP beforehand, so apMain() never allocates. */
#include "include/apic.h"
#include "include/cpu-impl.h"
#include "include/pte.h"
#include "include/smp-impl.h"

#include "acpi.h"
#include "atomic.h"
#include "cmdline.h"
#include "cpu-local.h"
#include "irq.h"
#include "kernel-boot.h"
#include "klog.h"
#include "kmalloc.h"
#include "spinlock-raw.h"
#include "panic.h"
#include "pmm.h"
#include "smp.h"
#include "timekeeping.h"
#include "vmalloc.h"

#include <arch/cpu-init.h>
#include <arch/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

#define MSR_IA32_EFER   0xC0000080u
#define EFER_LMA        (1ULL << 10)
#define CR4_PGE         (1ULL << 7)
#define CR4_UMIP        (1ULL << 11)
#define CR4_SMEP        (1ULL << 20)
#define CR4_SMAP        (1ULL << 21)
#define MSR_MTRR_CAP    0xFEu
#define MSR_MTRR_DEF    0x2FFu
#define MSR_MTRR_PHYS0  0x200u
#define MSR_MTRR_FIX64K 0x250u
#define MSR_MTRR_FIX16K 0x258u
#define MSR_MTRR_FIX4K0 0x268u

#define ICR_INIT  0x4500u /* INIT, assert, edge-ignored: the SDM's MP initialization sequence */
#define ICR_FIXED 0x4000u /* fixed delivery, assert */
#define ICR_NMI   0x4400u
#define ICR_SIPI  0x4600u /* | startup page number (the trampoline's physical page >> 12) */

#define AP_STACK_SIZE        0x4000u
#define AP_ONLINE_TIMEOUT_NS 2000000000ull /* D-193 */
#define AP_INIT_DELAY_NS     10000000ull   /* the SDM's 10 ms after INIT */
#define AP_SIPI_DELAY_NS     200000ull     /* and 200 us between SIPIs */
#define AP_PARK_DELAY_NS     10000000ull
#define AP_DEBUG_BSP_LAG_NS  300000000ull /* D-204: KERNEL_DEBUG only; below TSC_WAIT_NS */

#define MTRR_VAR_MAX 16u

/* The boot CPU's control-register and MTRR state, which every AP must reproduce (D-194). */
typedef struct {
    uint64_t cr0, cr4, efer;
    bool mtrrValid;
    uint64_t mtrrCap, mtrrDef;
    uint32_t varCount;
    uint64_t var[MTRR_VAR_MAX * 2];
    uint64_t fix[11];
} BspCpuState;

static BspCpuState bspState;
static const AcpiMadtInfo *madtInfo;
static uint64_t trampPhys;

#ifdef KERNEL_DEBUG
/* KERNEL_DEBUG only: the `smp-ap-early-panic-probe` cmdline token makes cpu 1 panic in apMain()
 * before its local APIC is set up (tests/harness/smp-panic-check.sh): the panic's stop IPI must
 * still reach the BSP, and exactly one report must print. Set by smpInit() before any AP starts. */
static bool apEarlyPanicProbe;
/* KERNEL_DEBUG only: the `smp-ap-stall-probe` token makes the AP with APIC id 1 spin for 3 s in
 * apMain() right after it entered (`smp-ap-stall-tsc-probe`: after the TSC check, just before its
 * claim), past the BSP's 2 s deadline, so the BSP's give-up path runs on every such boot
 * (tests/harness/smp-panic-check.sh). The BSP then also delays that AP's park INIT by
 * AP_STALL_INIT_LAG_NS, past the end of the stall: the AP's own half of the D-206 handshake, not
 * the INIT, must keep it out of the online mask. */
#define AP_STALL_NONE        0u
#define AP_STALL_ENTERED     1u
#define AP_STALL_TSC         2u
#define AP_STALL_NS          3000000000ull
#define AP_STALL_INIT_LAG_NS 2000000000ull
static uint32_t apStallProbe;
/* KERNEL_DEBUG only: `smp-bsp-giveup-lag-probe` (with smp-ap-stall-tsc-probe) delays the BSP
 * between reading an AP's stage and its give-up compare-exchange, so the AP's claim lands inside
 * that window: a plain store there would INIT-park a published AP (the BSP half of D-206). */
#define BSP_GIVE_UP_LAG_NS 1500000000ull
static bool bspGiveUpLagProbe;
#endif

static void *hhdm(uint64_t phys) {
    return (void *)(uintptr_t)(pmmHhdmBase() + phys);
}

static void delayNs(uint64_t ns) {
    uint64_t end = timeMonotonicNs() + ns;
    while (timeMonotonicNs() < end) {
        archPause();
    }
}

/* --- BSP state snapshot and the MTRR comparison ------------------------------------------------
 */

static const uint32_t fixedMtrrMsrs[11] = {MSR_MTRR_FIX64K,
                                           MSR_MTRR_FIX16K,
                                           0x259,
                                           MSR_MTRR_FIX4K0,
                                           MSR_MTRR_FIX4K0 + 1,
                                           0x26A,
                                           0x26B,
                                           0x26C,
                                           0x26D,
                                           0x26E,
                                           0x26F};

static bool archCpuIsHypervisor(void) {
    uint32_t r[4];
    archCpuid(1, 0, r);
    return (r[2] >> 31) & 1u;
}

static bool cpuHasMtrr(void) {
    uint32_t r[4];
    archCpuid(1, 0, r);
    return (r[3] >> 12) & 1u;
}

static void mtrrCapture(BspCpuState *st) {
    st->mtrrValid = false;
    if (!cpuHasMtrr()) {
        return;
    }
    st->mtrrCap = archRdmsr(MSR_MTRR_CAP);
    st->mtrrDef = archRdmsr(MSR_MTRR_DEF);
    st->varCount = (uint32_t)(st->mtrrCap & 0xFFu);
    if (st->varCount > MTRR_VAR_MAX) {
        st->varCount = MTRR_VAR_MAX;
    }
    for (uint32_t i = 0; i < st->varCount * 2; i++) {
        st->var[i] = archRdmsr(MSR_MTRR_PHYS0 + i);
    }
    if (st->mtrrCap & (1ULL << 8)) {
        for (uint32_t i = 0; i < 11; i++) {
            st->fix[i] = archRdmsr(fixedMtrrMsrs[i]);
        }
    }
    st->mtrrValid = true;
}

/* True when this CPU's MTRRs match the BSP's (variable registers compared by value; an unused
 * pair's mask is 0 on both). */
static bool mtrrMatchesBsp(const BspCpuState *st) {
    if (!st->mtrrValid) {
        return true;
    }
    if (!cpuHasMtrr() || archRdmsr(MSR_MTRR_CAP) != st->mtrrCap ||
        archRdmsr(MSR_MTRR_DEF) != st->mtrrDef) {
        return false;
    }
    for (uint32_t i = 0; i < st->varCount * 2; i++) {
        if (archRdmsr(MSR_MTRR_PHYS0 + i) != st->var[i]) {
            return false;
        }
    }
    if (st->mtrrCap & (1ULL << 8)) {
        for (uint32_t i = 0; i < 11; i++) {
            if (archRdmsr(fixedMtrrMsrs[i]) != st->fix[i]) {
                return false;
            }
        }
    }
    return true;
}

/* Programs this CPU's MTRRs from the BSP's snapshot with the SDM's MP update procedure (Vol 3A
 * §11.11.8): no-fill cache mode, flush, MTRRs off, write, flush, MTRRs on, restore. Needed because
 * QEMU resets the MTRRs on INIT (real hardware preserves them, so there this is never reached).
 * IF=0 on entry. */
static void mtrrApplyBsp(const BspCpuState *st) {
    uint64_t cr0 = archReadCr0();
    uint64_t cr4 = archReadCr4();
    archWriteCr0((cr0 | (1ULL << 30)) & ~(1ULL << 29)); /* CD=1, NW=0 */
    archWbinvd();
    archWriteCr4(cr4 & ~CR4_PGE); /* PGE 1->0 flushes the whole TLB, global entries included */
    archWrmsr(MSR_MTRR_DEF, 0);
    for (uint32_t i = 0; i < st->varCount * 2; i++) {
        archWrmsr(MSR_MTRR_PHYS0 + i, st->var[i]);
    }
    if (st->mtrrCap & (1ULL << 8)) {
        for (uint32_t i = 0; i < 11; i++) {
            archWrmsr(fixedMtrrMsrs[i], st->fix[i]);
        }
    }
    archWbinvd();
    archWrmsr(MSR_MTRR_DEF, st->mtrrDef);
    archWriteCr4(cr4);
    archWriteCr0(cr0);
}

/* --- cross-CPU TSC check (D-198) -----------------------------------------------------------------
 */

/* The BSP and one AP run this protocol in lockstep (both IF=0) while the AP boots, in three phases
 * separated by barriers: (A) both CPUs alternately stamp a shared `last` TSC value under one lock,
 * and any reading below the previous one is a warp; (B) only if A saw a warp, 16 ping-pong rounds
 * estimate how far the AP's TSC is ahead, keeping the round with the smallest round trip; (C) the
 * check runs again with the estimate applied. The estimate becomes the AP's `tscOffset`, which
 * timeMonotonicNs() subtracts (the hardware timer path uses the raw local TSC, so offsets never
 * touch it). IA32_TSC_ADJUST is not written (D-198). Every wait is bounded; on a timeout both
 * sides give up and the AP keeps offset 0. */
#define TSC_WARP_ITER   4000u
#define TSC_PING_ROUNDS 16u
#define TSC_WAIT_NS     500000000ull

typedef struct {
    RawSpinlock lock;
    uint64_t last;
    uint32_t warps;
    uint32_t barrier[4];
    uint32_t failed;
    uint32_t pingSeq, pongSeq;
    uint64_t pongTsc;
    int64_t offset;   /* the estimate: how far the AP's TSC is ahead of the BSP's */
    int64_t fakeSkew; /* ktest only: added to the AP's readings, to exercise the estimator */
} TscSync;

static TscSync tscSync;

typedef struct {
    uint32_t warpsBefore, warpsAfter;
    int64_t offset;
    bool ok;
} TscSyncResult;

/* Spins until `*p >= want`; false on a timeout or if the other side already gave up. */
static bool tscWait(TscSync *s, const uint32_t *p, uint32_t want) {
    uint64_t deadline = timeMonotonicNs() + TSC_WAIT_NS;
    while (ATOMIC_LOAD(p, MEM_ACQUIRE) < want) {
        if (ATOMIC_LOAD(&s->failed, MEM_ACQUIRE) != 0) {
            return false;
        }
        if (timeMonotonicNs() >= deadline) {
            ATOMIC_STORE(&s->failed, 1, MEM_RELEASE);
            return false;
        }
        archPause();
    }
    return true;
}

static bool tscBarrier(TscSync *s, uint32_t n) {
    ATOMIC_FETCH_ADD(&s->barrier[n], 1, MEM_SEQ_CST);
    return tscWait(s, &s->barrier[n], 2);
}

/* One side of the warp check: stamps the shared `last` for TSC_WARP_ITER rounds (or about 10 ms),
 * reading its own TSC plus `adj`. Adds the warps it saw to s->warps. */
static void tscWarpLoop(TscSync *s, int64_t adj, uint64_t spanTicks) {
    uint64_t start = archReadTscOrdered();
    uint32_t warps = 0;
    for (uint32_t i = 0; i < TSC_WARP_ITER; i++) {
        rawSpinLock(&s->lock);
        uint64_t prev = s->last;
        uint64_t now = archReadTscOrdered() + (uint64_t)adj;
        s->last = now;
        rawSpinUnlock(&s->lock);
        if (now < prev) {
            warps++;
        }
        if (archReadTscOrdered() - start > spanTicks) {
            break;
        }
    }
    ATOMIC_FETCH_ADD(&s->warps, warps, MEM_SEQ_CST);
}

/* BSP half. Runs with IF=0 once the AP is waiting (SMP_STAGE_TSC). */
static void tscSyncBsp(TscSync *s, TscSyncResult *out) {
    *out = (TscSyncResult){0};
    uint64_t span = timeTscHz() / 100;
    if (!tscBarrier(s, 0)) { /* both sides are ready */
        return;
    }
    tscWarpLoop(s, 0, span);
    if (!tscBarrier(s, 1)) {
        return;
    }
    out->warpsBefore = ATOMIC_LOAD(&s->warps, MEM_ACQUIRE);
    if (out->warpsBefore != 0) {
        uint64_t t0[TSC_PING_ROUNDS], t1[TSC_PING_ROUNDS], t2[TSC_PING_ROUNDS];
        for (uint32_t r = 0; r < TSC_PING_ROUNDS; r++) {
            t0[r] = archReadTscOrdered();
            ATOMIC_STORE(&s->pingSeq, r + 1, MEM_SEQ_CST);
            if (!tscWait(s, &s->pongSeq, r + 1)) {
                return;
            }
            t2[r] = archReadTscOrdered();
            t1[r] = s->pongTsc;
        }
        int64_t off;
        uint64_t rtt;
        if (timeTscPingpongBest(t0, t1, t2, TSC_PING_ROUNDS, &off, &rtt) != STATUS_OK) {
            ATOMIC_STORE(&s->failed, 1, MEM_RELEASE);
            return;
        }
        s->offset = off;
        out->offset = off;
        ATOMIC_STORE(&s->warps, 0, MEM_SEQ_CST);
    }
    if (!tscBarrier(s, 2)) {
        return;
    }
    if (out->warpsBefore != 0) {
        tscWarpLoop(s, 0, span);
    }
    if (!tscBarrier(s, 3)) { /* both rechecks are done before anyone reads the count */
        return;
    }
    out->warpsAfter = ATOMIC_LOAD(&s->warps, MEM_ACQUIRE);
    out->ok = true;
}

/* AP half; `cl` is this CPU. `out->ok` is set when the check completed (the estimate is in
 * out->offset, not yet applied); a timeout leaves it clear. */
static void tscSyncAp(TscSync *s, const CpuLocal *cl, TscSyncResult *out) {
    *out = (TscSyncResult){0};
    uint64_t span = timeTscHz() / 100;
    int64_t skew = s->fakeSkew;
    int64_t adj = skew - cl->arch.tscOffset;
    if (!tscBarrier(s, 0)) {
        return;
    }
    tscWarpLoop(s, adj, span);
    if (!tscBarrier(s, 1)) {
        return;
    }
    uint32_t warps = ATOMIC_LOAD(&s->warps, MEM_ACQUIRE);
    out->warpsBefore = warps;
    if (warps != 0) {
        for (uint32_t r = 0; r < TSC_PING_ROUNDS; r++) {
            if (!tscWait(s, &s->pingSeq, r + 1)) {
                return;
            }
            s->pongTsc = archReadTscOrdered() + (uint64_t)adj;
            ATOMIC_STORE(&s->pongSeq, r + 1, MEM_SEQ_CST);
        }
    }
    if (!tscBarrier(s, 2)) {
        return;
    }
    if (warps != 0) {
        out->offset = s->offset;
        tscWarpLoop(s, adj - s->offset, span);
    }
    if (!tscBarrier(s, 3)) {
        return;
    }
    out->warpsAfter = ATOMIC_LOAD(&s->warps, MEM_ACQUIRE);
    out->ok = true;
}

typedef struct {
    TscSyncResult res;
} TscTestCtx;

static void tscTestApFn(void *arg) {
    TscTestCtx *c = arg;
    uint64_t f = archIrqSave();
    tscSyncAp(&tscSync, cpuLocal(), &c->res);
    archIrqRestore(f);
}

bool archTscSyncTest(uint32_t cpu, int64_t skew, uint32_t *warpsBefore, uint32_t *warpsAfter,
                     int64_t *estimate) {
    memset(&tscSync, 0, sizeof(tscSync));
    tscSync.fakeSkew = skew;
    TscTestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    SmpWork w = {.fn = tscTestApFn, .arg = &ctx, .remaining = 1};
    smpWorkPost(cpu, &w);
    TscSyncResult bsp;
    uint64_t f = archIrqSave();
    tscSyncBsp(&tscSync, &bsp);
    archIrqRestore(f);
    smpWorkWait(&w);
    *warpsBefore = bsp.warpsBefore;
    *warpsAfter = bsp.warpsAfter;
    *estimate = bsp.offset;
    return bsp.ok && ctx.res.ok;
}

uint64_t archSmpTrampolinePage(void) {
    return trampPhys;
}

/* --- the AP side -------------------------------------------------------------------------------
 */

/* Parks an AP that cannot continue: the BSP times it out. IRQs stay off. */
static _Noreturn void apFail(CpuLocal *cl) {
    ATOMIC_STORE(&cl->bootStage, SMP_STAGE_FAILED, MEM_RELEASE);
    archHaltForever();
}

/* Moves this AP's stage from `from` to `to`, or parks it if the BSP gave up on it meanwhile (its
 * compare-exchange to FAILED won, D-206). Every step of the AP's stage is one of these: a plain
 * store would overwrite the BSP's FAILED and let a given-up AP go on to claim and publish itself
 * if the BSP's park INIT arrived late. */
static void apAdvanceStage(CpuLocal *cl, uint32_t from, uint32_t to) {
    if (!ATOMIC_CMPXCHG(&cl->bootStage, &from, to, MEM_ACQ_REL, MEM_ACQUIRE)) {
        archHaltForever(); /* the stage stays FAILED */
    }
}

/* The idle loop of an AP with no scheduler (M4 replaces it): `sti; hlt` in one asm statement, so
 * an interrupt that arrives between the two cannot be lost (the STI shadow covers the hlt). */
static _Noreturn void smpIdleLoop(void) {
    for (;;) {
        archDisableInterrupts();
        if (smpWorkRunPending()) {
            continue;
        }
        __asm__ volatile("sti\n\thlt" ::: "memory");
    }
}

/* Runs on the AP, IF=0, on its own stack (ap-entry.asm), with GS already set. Allocates nothing. */
__attribute__((used)) _Noreturn void apMain(CpuLocal *cl) {
    apAdvanceStage(cl, SMP_STAGE_NONE, SMP_STAGE_ENTERED);
    /* Before anything that can panic: a panic's stop IPI goes out in the BSP's APIC mode. */
    lapicApMatchMode();
    archCpuInitAp(cl);

    /* Control registers first (CR4.PGE is what the PAT procedure toggles), reproducing the BSP's.
     * A CR4 bit this CPU's CPUID does not offer is dropped rather than faulting on the write. */
    uint32_t r0[4], r7[4] = {0, 0, 0, 0};
    archCpuid(0, 0, r0);
    if (r0[0] >= 7) {
        archCpuid(7, 0, r7);
    }
    uint64_t cr4 = bspState.cr4;
    uint64_t dropped = 0;
    if (!(r7[1] & (1u << 7)) && (cr4 & CR4_SMEP)) {
        dropped |= CR4_SMEP;
    }
    if (!(r7[1] & (1u << 20)) && (cr4 & CR4_SMAP)) {
        dropped |= CR4_SMAP;
    }
    if (!(r7[2] & (1u << 2)) && (cr4 & CR4_UMIP)) {
        dropped |= CR4_UMIP;
    }
    cr4 &= ~dropped;
    archWriteCr0(bspState.cr0);
    archWriteCr4(cr4);
    archWrmsr(MSR_IA32_EFER, archRdmsr(MSR_IA32_EFER) | (bspState.efer & ~EFER_LMA));

    bool mtrrSynced = false;
    if (!mtrrMatchesBsp(&bspState)) {
        mtrrApplyBsp(&bspState);
        mtrrSynced = true;
    }
    archPatProgramThisCpu(); /* before any klog: the framebuffer is a WC mapping */

#ifdef KERNEL_DEBUG
    if (apEarlyPanicProbe && cl->cpuId == 1) {
        panic("smp-ap-early-panic-probe: cpu %u panics before its local APIC is set up", cl->cpuId);
    }
#endif
#ifdef KERNEL_DEBUG
    if (apStallProbe == AP_STALL_ENTERED && cl->apicId == 1) {
        delayNs(AP_STALL_NS);
    }
#endif
    lapicInitAp(madtInfo);
    if (lapicId() != cl->apicId) {
        klogWrite(KLOG_ERROR, "smp", "cpu %u: local APIC id %u, expected %u", cl->cpuId, lapicId(),
                  cl->apicId);
        apFail(cl);
    }
    /* No klog before the AP is published (D-206): the BSP may INIT an AP it has given up on, and an
     * INIT that lands inside klog's critical section would leave its lock held for good. What there
     * is to say is kept and logged after ONLINE. */
    bool mtrrCopied = mtrrSynced && mtrrMatchesBsp(&bspState);
    bool mtrrDiffers = mtrrSynced && !mtrrCopied;

    lapicTimerCpuSetup(); /* this CPU's LAPIC timer; its queue (cl->timer) was attached by the BSP
                           */

    /* Cross-CPU TSC check against the BSP, which runs its half while we wait in SMP_STAGE_TSC. */
    apAdvanceStage(cl, SMP_STAGE_ENTERED, SMP_STAGE_TSC);
    TscSyncResult tr;
    tscSyncAp(&tscSync, cl, &tr);
    if (tr.ok && tr.offset != 0) {
        cl->arch.tscOffset = tr.offset; /* timeMonotonicNs() on this CPU subtracts it from now on */
    }
#ifdef KERNEL_DEBUG
    if (apStallProbe == AP_STALL_TSC && cl->apicId == 1) {
        delayNs(AP_STALL_NS);
    }
#endif

    /* Claim the right to publish (D-206): the BSP gives up on an AP with a compare-exchange of the
     * same word from a pre-publication stage to FAILED, so exactly one of us wins. Losing means the
     * BSP already decided (and is about to INIT us): park instead of appearing in the mask. */
    apAdvanceStage(cl, SMP_STAGE_TSC, SMP_STAGE_PUBLISHING);

    /* Publish: the CpuLocal first, then the online mask, then a full local TLB flush (a shootdown
     * that missed this CPU because it was not in the mask yet must not leave a stale entry behind,
     * D-194), and only then tell the BSP. */
    ATOMIC_STORE(&cpuTable[cl->cpuId], cl, MEM_RELEASE);
    smpMarkOnline(cl->cpuId);
    uint64_t cr4Now = archReadCr4();
    archWriteCr4(cr4Now & ~CR4_PGE);
    archWriteCr4(cr4Now);
    /* Nothing that can block (the klog lock) between the online mask and ONLINE: the BSP waits for
     * a published AP however long it takes (smpApVerdict), so that stretch must stay this short. */
    ATOMIC_STORE(&cl->bootStage, SMP_STAGE_ONLINE, MEM_RELEASE);
    if (dropped != 0) {
        klogWrite(KLOG_WARN, "smp", "cpu %u: CR4 bits 0x%llx unsupported here, dropped", cl->cpuId,
                  (unsigned long long)dropped);
    }
    if (mtrrCopied) {
        klogWrite(KLOG_INFO, "smp", "cpu %u: MTRRs copied from the boot CPU", cl->cpuId);
    } else if (mtrrDiffers) {
        klogWrite(KLOG_WARN, "smp", "cpu %u: MTRRs differ from the boot CPU's", cl->cpuId);
    }
    klogWrite(KLOG_INFO, "smp", "cpu %u apic-id=%u online", cl->cpuId, cl->apicId);

    smpIdleLoop();
}

void archSmpSendIpi(uint32_t cpuId, uint32_t vector) {
    CpuLocal *cl = cpuLocalOf(cpuId);
    if (cl == NULL) {
        panicBug("archSmpSendIpi: cpu %u is not online", cpuId);
    }
    lapicSendIpi(cl->apicId, ICR_FIXED | vector);
}

void archSmpSendNmi(uint32_t cpuId) {
    CpuLocal *cl = cpuLocalOf(cpuId);
    if (cl != NULL) {
        lapicSendIpi(cl->apicId, ICR_NMI);
    }
}

/* --- the BSP side ------------------------------------------------------------------------------
 */

typedef struct {
    uint64_t pml4, pdpt, pd, pt; /* physical addresses, all below 4 GiB */
} TrampTables;
static TrampTables tramp;

static bool allocTrampTables(void) {
    uint64_t *slots[4] = {&tramp.pml4, &tramp.pdpt, &tramp.pd, &tramp.pt};
    for (uint32_t i = 0; i < 4; i++) {
        Page *p;
        if (pmmAllocPages(0, PMM_FLAG_DMA32 | PMM_FLAG_ZERO, &p) != STATUS_OK) {
            for (uint32_t j = 0; j < i; j++) {
                pmmFreePages(pmmPhysToPage(*slots[j]), 0);
            }
            return false;
        }
        *slots[i] = pmmPageToPhys(p);
    }
    return true;
}

static void freeTrampTables(void) {
    uint64_t *slots[4] = {&tramp.pml4, &tramp.pdpt, &tramp.pd, &tramp.pt};
    for (uint32_t i = 0; i < 4; i++) {
        pmmFreePages(pmmPhysToPage(*slots[i]), 0);
        *slots[i] = 0;
    }
}

/* The AP's temporary tables: the kernel half is a by-value copy of the live kernel PML4's upper 256
 * entries (so the AP's stack and the kernel image are mapped), the low half maps only the
 * trampoline page, identity, read-only and executable (D-192: the HHDM alias stays RW; this PML4
 * is not the kernel's, which archPagingVerifyWx walks). All 256 kernel-half PML4 slots are
 * allocated eagerly at boot and never rewritten (D-086), so the copy is complete whenever it is
 * made; it is rebuilt before each AP only to start every AP from clean tables. */
static void buildTrampTables(void) {
    uint64_t *pml4 = hhdm(tramp.pml4);
    uint64_t *pdpt = hhdm(tramp.pdpt);
    uint64_t *pd = hhdm(tramp.pd);
    uint64_t *pt = hhdm(tramp.pt);
    const uint64_t *kpml4 = hhdm(archReadCr3() & X86_PTE_ADDR_MASK);
    for (uint32_t i = 0; i < 256; i++) {
        pml4[i] = 0;
    }
    for (uint32_t i = 256; i < 512; i++) {
        pml4[i] = kpml4[i];
    }
    for (uint32_t i = 0; i < 512; i++) {
        pdpt[i] = 0;
        pd[i] = 0;
        pt[i] = 0;
    }
    pml4[0] = tramp.pdpt | X86_PTE_P | X86_PTE_W;
    pdpt[0] = tramp.pd | X86_PTE_P | X86_PTE_W;
    pd[0] = tramp.pt | X86_PTE_P | X86_PTE_W;
    pt[trampPhys >> 12] = trampPhys | X86_PTE_P; /* R-X: no W, no NX, not global */
}

static ApTrampData *trampData(void) {
    return (ApTrampData *)((uint8_t *)hhdm(trampPhys) + AP_TRAMP_DATA_OFF);
}

/* The AP writes `claimed` and `stage` while the BSP polls them: read them as plain aligned words
 * (the struct is packed, so the compiler cannot prove the alignment). */
static uint32_t trampWord(size_t off) {
    return *(volatile uint32_t *)((uint8_t *)trampData() + off);
}

/* Copies the blob into the trampoline page and fills in everything the AP needs. */
static void prepareTrampoline(const CpuLocal *cl, uint32_t apicId, bool leafB) {
    uint8_t *page = hhdm(trampPhys);
    memcpy(page, apTrampolineBlob, AP_TRAMP_PAGE_SIZE);
    ApTrampData *d = trampData();
    d->gdtBase += (uint32_t)trampPhys;
    d->pm32Off += (uint32_t)trampPhys;
    d->lm64Off += (uint32_t)trampPhys;
    d->claimed = 0;
    d->stage = AP_TRAMP_STAGE_NONE;
    d->targetApicId = apicId;
    d->useLeafB = leafB ? 1 : 0;
    d->pml4Phys = (uint32_t)tramp.pml4;
    d->entry64 = (uint64_t)(uintptr_t)apEntry64;
    d->stackTop = cl->arch.stackTop;
    d->cpuLocal = (uint64_t)(uintptr_t)cl;
    d->kernelCr3 = archReadCr3();
}

/* Allocates one AP's CpuLocal, stacks and per-CPU subsystem blobs. NULL on failure, with everything
 * freed that is not yet registered: a pmm/slab blob that pmmCpuAttach()/slabCpuAttach() accepted
 * stays in that subsystem's registry for good (there is no detach), so from then on it is leaked
 * (as is an AP that does not come online, D-193). The CpuLocal and the stacks are only referenced
 * from here and are always freed. */
static CpuLocal *allocAp(uint32_t cpuId, uint32_t apicId) {
    CpuLocal *cl = vmalloc(sizeof(*cl), VMALLOC_ZERO);
    uint8_t *kstack = vmalloc(AP_STACK_SIZE, 0);
    uint8_t *ist[3];
    bool ok = cl != NULL && kstack != NULL;
    for (uint32_t i = 0; i < 3; i++) {
        ist[i] = vmalloc(AP_STACK_SIZE, 0);
        ok = ok && ist[i] != NULL;
    }
    void *pmmBlob = ok ? kmalloc(pmmCpuBlobSize(), KMALLOC_ZERO) : NULL;
    void *slabBlob = ok ? vmalloc(slabCpuBlobSize(), VMALLOC_ZERO) : NULL;
    ok = ok && pmmBlob != NULL && slabBlob != NULL;
    if (ok) {
        cl->self = cl;
        cl->cpuId = cpuId;
        cl->apicId = apicId;
        cl->arch.stackBottom = (uint64_t)(uintptr_t)kstack;
        cl->arch.stackTop = (uint64_t)(uintptr_t)kstack + AP_STACK_SIZE;
        for (uint32_t i = 0; i < 3; i++) {
            cl->arch.istBottom[i] = (uint64_t)(uintptr_t)ist[i];
            cl->arch.istTop[i] = (uint64_t)(uintptr_t)ist[i] + AP_STACK_SIZE;
        }
        if (pmmCpuAttach(cl, pmmBlob) != STATUS_OK) {
            ok = false;
        } else {
            pmmBlob = NULL; /* registered: never freed */
            if (slabCpuAttach(cl, slabBlob) != STATUS_OK) {
                ok = false;
            } else {
                slabBlob = NULL;
                ok = timeCpuAttach(cl) == STATUS_OK;
            }
        }
    }
    if (ok) {
        return cl;
    }
    kfree(pmmBlob);
    vfree(slabBlob);
    for (uint32_t i = 0; i < 3; i++) {
        vfree(ist[i]);
    }
    vfree(kstack);
    vfree(cl);
    return NULL;
}

static void reportTscSync(const CpuLocal *cl, const TscSyncResult *tr) {
    if (!tr->ok) {
        klogWrite(KLOG_WARN, "smp", "cpu %u: the TSC check timed out; its clock is not compared",
                  cl->cpuId);
    } else if (tr->warpsBefore != 0) {
        /* Under a hypervisor a skew is the host's doing; on bare metal it is worth a warning. */
        bool bare = !archCpuIsHypervisor();
        bool unfixed = tr->warpsAfter >= tr->warpsBefore; /* the estimate did not help at all */
        klogWrite(bare && unfixed ? KLOG_WARN : KLOG_INFO, "smp",
                  "cpu %u: TSC ran %lld ticks ahead of the boot CPU's; offset applied (%u warps "
                  "before, %u after)",
                  cl->cpuId, (long long)tr->offset, (unsigned)tr->warpsBefore,
                  (unsigned)tr->warpsAfter);
    }
}

/* Starts one AP and waits for it (D-193). true once it is ONLINE. */
static bool bootAp(CpuLocal *cl) {
    /* The TSC check's shared state is reset BEFORE the AP is released: an AP can reach its half of
     * the check before the BSP gets here again, and a reset under it would wipe its barrier arrival
     * or the ticket lock it holds (and a stale state left by the previous AP would let it run ahead
     * through every barrier alone). */
    memset(&tscSync, 0, sizeof(tscSync));
    buildTrampTables();
    prepareTrampoline(cl, cl->apicId, lapicIsX2apic());
    ATOMIC_FENCE(MEM_SEQ_CST);

    lapicSendIpi(cl->apicId, ICR_INIT);
    delayNs(AP_INIT_DELAY_NS);
    uint32_t sipi = ICR_SIPI | (uint32_t)(trampPhys >> 12);
    lapicSendIpi(cl->apicId, sipi);
    delayNs(AP_SIPI_DELAY_NS);
    /* A SIPI to a CPU not waiting for one is ignored, so the second is harmless when the first
     * worked and the SDM's remedy when it was lost. */
    if (trampWord(offsetof(ApTrampData, claimed)) == 0) {
        lapicSendIpi(cl->apicId, sipi);
    }
#ifdef KERNEL_DEBUG
    /* D-204: debug builds play a BSP that lags its AP: it lets the AP run ahead to the TSC check
     * (bounded) before doing anything else, so state the BSP still touched after releasing the AP
     * would be caught on every debug boot rather than only on fast hardware. */
    uint64_t lagEnd = timeMonotonicNs() + AP_DEBUG_BSP_LAG_NS;
    while (ATOMIC_LOAD(&cl->bootStage, MEM_ACQUIRE) != SMP_STAGE_TSC &&
           ATOMIC_LOAD(&cl->bootStage, MEM_ACQUIRE) != SMP_STAGE_FAILED &&
           timeMonotonicNs() < lagEnd) {
        archPause();
    }
#endif

    bool tscDone = false;
    uint64_t deadline = timeMonotonicNs() + AP_ONLINE_TIMEOUT_NS;
    for (;;) {
        uint32_t stage = ATOMIC_LOAD(&cl->bootStage, MEM_ACQUIRE);
        bool published = (smpOnlineMask() & ((uint64_t)1 << cl->cpuId)) != 0;
        SmpApVerdict v = smpApVerdict(stage, published, timeMonotonicNs() >= deadline);
        if (v == SMP_AP_ONLINE) {
            if (!tscDone) { /* the AP timed out of the check alone: the boot CPU was too late */
                klogWrite(KLOG_WARN, "smp", "cpu %u: the TSC check was skipped (boot CPU late)",
                          cl->cpuId);
            }
            return true;
        }
        if (v == SMP_AP_WAIT && !tscDone && stage == SMP_STAGE_TSC) {
            tscDone = true;
            TscSyncResult tr;
            uint64_t f = archIrqSave();
            tscSyncBsp(&tscSync, &tr);
            archIrqRestore(f);
            reportTscSync(cl, &tr);
            continue; /* the check can take long (its waits are bounded at 500 ms each): the AP
                       * may have published meanwhile, so decide from fresh state */
        }
        if (v == SMP_AP_GIVE_UP) {
            /* The AP's claim to publish (TSC -> PUBLISHING) races with this: whoever changes the
             * stage first wins, and if the AP won it is now published or about to be. */
            uint32_t seen = stage;
#ifdef KERNEL_DEBUG
            if (bspGiveUpLagProbe && cl->apicId == 1) {
                /* Between reading the stage and the compare-exchange: the AP (stalled 3 s before
                 * its claim) claims in this window, and the exchange must then fail. */
                klogWrite(KLOG_INFO, "smp", "bsp-giveup-lag-probe: delaying the give-up exchange");
                delayNs(BSP_GIVE_UP_LAG_NS);
            }
#endif
            if (!ATOMIC_CMPXCHG(&cl->bootStage, &seen, SMP_STAGE_FAILED, MEM_ACQ_REL,
                                MEM_ACQUIRE)) {
                continue;
            }
            klogWrite(KLOG_WARN, "smp",
                      "cpu apic-id=%u did not come online (trampoline stage %u, boot stage %u)",
                      cl->apicId, (unsigned)trampWord(offsetof(ApTrampData, stage)),
                      (unsigned)stage);
#ifdef KERNEL_DEBUG
            if (apStallProbe != AP_STALL_NONE && cl->apicId == 1) {
                delayNs(AP_STALL_INIT_LAG_NS); /* a late INIT: the AP must park itself */
            }
#endif
            lapicSendIpi(cl->apicId, ICR_INIT); /* park it again */
            delayNs(AP_PARK_DELAY_NS);
            return false;
        }
        archPause();
    }
}

void smpInit(void) {
    for (uint32_t v = SMP_VECTOR_TLB; v <= SMP_VECTOR_STOP; v++) {
        Status st = irqRegister(v, smpIpiHandler, NULL);
        if (st != STATUS_OK) {
            panic("smp: cannot register IPI vector 0x%x (status %d)", (unsigned)v, (int)st);
        }
    }
    cpuLocalBsp.apicId = lapicId();
    cpuLocalBsp.bootStage = SMP_STAGE_ONLINE;
    const char *cmdline = kernelCmdline();
#ifdef KERNEL_DEBUG
    apEarlyPanicProbe = cmdlineHasToken(cmdline, "smp-ap-early-panic-probe");
    bspGiveUpLagProbe = cmdlineHasToken(cmdline, "smp-bsp-giveup-lag-probe");
    apStallProbe = cmdlineHasToken(cmdline, "smp-ap-stall-probe")       ? AP_STALL_ENTERED
                   : cmdlineHasToken(cmdline, "smp-ap-stall-tsc-probe") ? AP_STALL_TSC
                                                                        : AP_STALL_NONE;
#endif
    bool present, invalid;
    uint32_t maxTotal = smpParseCpusOption(cmdline, CPU_MAX, &present, &invalid);
    if (invalid) {
        klogWrite(KLOG_WARN, "smp", "ignoring invalid cpus= (want a number from 1 to %u)",
                  (unsigned)CPU_MAX);
    }

    const AcpiInfo *info = acpiGetInfo();
    if (info == NULL || info->madtStatus != STATUS_OK) {
        klogWrite(KLOG_INFO, "smp", "no MADT: staying on the boot CPU");
        klogWrite(KLOG_INFO, "smp", "%u cpus online", smpOnlineCount());
        return;
    }
    madtInfo = &info->madt;

    uint32_t apIds[CPU_MAX];
    uint32_t skipped;
    uint32_t apCount = smpSelectAps(madtInfo->cpus, madtInfo->cpuCount, lapicId(), lapicIsX2apic(),
                                    maxTotal, apIds, CPU_MAX - 1, &skipped);
    if (skipped != 0) {
        klogWrite(KLOG_INFO, "smp", "%u cpus have APIC ids above 254 and need x2APIC; not started",
                  skipped);
    }
    uint32_t enabledCpus = 0;
    for (uint32_t i = 0; i < madtInfo->cpuCount; i++) {
        enabledCpus += (madtInfo->cpus[i].flags & 1u) != 0;
    }
    if (present && !invalid && maxTotal < enabledCpus) {
        klogWrite(KLOG_INFO, "smp", "cpus=%u: limiting to %u of %u enabled cpus", maxTotal,
                  maxTotal, enabledCpus);
    }

    if (apCount != 0) {
        uint32_t mapCount;
        const BootMemRegion *map = kernelBootMemMap(&mapCount);
        if (!smpPickTrampolinePage(map, mapCount, &trampPhys)) {
            klogWrite(KLOG_WARN, "smp", "no usable page below 640 KiB for the startup trampoline");
            apCount = 0;
        } else if (!allocTrampTables()) {
            klogWrite(KLOG_WARN, "smp", "cannot allocate the trampoline page tables");
            apCount = 0;
        }
    }

    if (apCount != 0) {
        bspState.cr0 = archReadCr0();
        bspState.cr4 = archReadCr4();
        bspState.efer = archRdmsr(MSR_IA32_EFER);
        mtrrCapture(&bspState);
        klogWrite(KLOG_INFO, "smp", "trampoline page=0x%llx", (unsigned long long)trampPhys);

        uint32_t nextId = 1;
        for (uint32_t i = 0; i < apCount; i++) {
            CpuLocal *cl = allocAp(nextId, apIds[i]);
            if (cl == NULL) {
                klogWrite(KLOG_WARN, "smp", "out of memory for cpu apic-id=%u; stopping", apIds[i]);
                break;
            }
            if (bootAp(cl)) {
                nextId++; /* a failed AP's id is reused, its memory leaked */
            }
        }
        freeTrampTables();
        memset(hhdm(trampPhys), 0, AP_TRAMP_PAGE_SIZE);
    }
    klogWrite(KLOG_INFO, "smp", "%u cpus online", smpOnlineCount());
}
