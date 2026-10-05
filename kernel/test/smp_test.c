/* ktests for per-CPU data and SMP bring-up (M3.5, D-190..). */
#include "acpi.h"
#include "atomic.h"
#include "cpu-local.h"
#include "kernel-boot.h"
#include "irq.h"
#include "klog.h"
#include "kmalloc.h"
#include "ktest.h"
#include "pmm.h"
#include "preempt.h"
#include "smp.h"
#include "spinlock.h"
#include "timekeeping.h"
#include "vmalloc.h"
#include "vmm.h"

#include <arch/cpu.h>
#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

/* Every CPU the MADT lists as enabled (up to `cpus=` and CPU_MAX) came online, each with a dense
 * id, its own CpuLocal and a distinct APIC id the MADT knows. */
KTEST(smp_online_matches_madt) {
    const AcpiInfo *info = acpiGetInfo();
    KTEST_ASSERT(info != NULL && info->madtStatus == STATUS_OK);
    uint32_t enabled = 0;
    for (uint32_t i = 0; i < info->madt.cpuCount; i++) {
        if (info->madt.cpus[i].flags & 1u) {
            enabled++;
        }
    }
    bool present, invalid;
    uint32_t limit = smpParseCpusOption(kernelCmdline(), CPU_MAX, &present, &invalid);
    uint32_t want = enabled < limit ? enabled : limit;
    KTEST_ASSERT_EQ(smpOnlineCount(), want);
    KTEST_ASSERT_EQ(smpOnlineMask(), want >= 64 ? ~0ULL : ((1ULL << want) - 1));

    for (uint32_t id = 0; id < want; id++) {
        CpuLocal *cl = cpuLocalOf(id);
        KTEST_ASSERT(cl != NULL);
        KTEST_ASSERT(cl->self == cl);
        KTEST_ASSERT_EQ(cl->cpuId, id);
        KTEST_ASSERT_EQ(cl->bootStage, SMP_STAGE_ONLINE);
        bool listed = false;
        for (uint32_t i = 0; i < info->madt.cpuCount; i++) {
            listed = listed || (info->madt.cpus[i].apicId == cl->apicId &&
                                (info->madt.cpus[i].flags & 1u) != 0);
        }
        KTEST_ASSERT(listed);
        for (uint32_t other = 0; other < id; other++) {
            KTEST_ASSERT(cpuLocalOf(other)->apicId != cl->apicId);
        }
    }
    KTEST_ASSERT(cpuLocalOf(want) == NULL);
}

/* --- call-function and TLB shootdown (D-195, D-196) --------------------------------------------
 */

typedef struct {
    uint32_t calls;
    uint64_t cpuMask;
    uint32_t badContext; /* calls that ran with IF=1 or outside a handler, or on the wrong CPU */
} CallState;

static void callCounter(void *arg) {
    CallState *s = arg;
    ATOMIC_FETCH_ADD(&s->calls, 1, MEM_SEQ_CST);
    ATOMIC_FETCH_OR(&s->cpuMask, (uint64_t)1 << smpThisCpu(), MEM_SEQ_CST);
    if (archInterruptsEnabled()) {
        ATOMIC_FETCH_ADD(&s->badContext, 1, MEM_SEQ_CST);
    }
}

/* Every online CPU (the caller's included) runs the function exactly once per call, in IRQ context
 * (ROADMAP M3.5 item 7: "every CPU runs a call-function and increments a counter"). */
KTEST(smp_call_function_all_cpus) {
    CallState s = {0};
    uint32_t n = smpOnlineCount();
    for (uint32_t round = 1; round <= 1000; round++) {
        smpCallFunction(smpOnlineMask(), callCounter, &s);
        KTEST_ASSERT_EQ(ATOMIC_LOAD(&s.calls, MEM_SEQ_CST), round * n);
    }
    KTEST_ASSERT_EQ(s.cpuMask, smpOnlineMask());
    KTEST_ASSERT_EQ(s.badContext, 0);

    /* A mask naming offline CPUs ignores them; an empty mask runs nothing. */
    uint32_t before = s.calls;
    smpCallFunction(0, callCounter, &s);
    smpCallFunction((uint64_t)1 << 63, callCounter, &s);
    KTEST_ASSERT_EQ(s.calls, before);
    /* Only the caller. */
    smpCallFunction((uint64_t)1 << smpThisCpu(), callCounter, &s);
    KTEST_ASSERT_EQ(s.calls, before + 1);
}

static void callBadArgsTrigger(void *arg) {
    (void)arg;
    smpCallFunction(smpOnlineMask(), callCounter, NULL);
}

/* With another CPU in the mask, calling with IF=0 would risk a deadlock: it is refused (panicBug),
 * not attempted. Only meaningful with more than one CPU online. */
KTEST(smp_call_function_refused_with_irqs_off) {
    if (smpOnlineCount() < 2) {
        return;
    }
    TrapCatchInfo info;
    CallState s = {0};
    uint64_t f = archIrqSave();
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, callBadArgsTrigger, &s, &info);
    archIrqRestore(f);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(s.calls, 0);
}

static void callCountingTrigger(void *arg) {
    smpCallFunction(smpOnlineMask(), callCounter, arg);
}

static Spinlock callHeldLock = SPINLOCK_INIT("smp-test-call-held");

/* D-207 (2): with another CPU in the mask, calling with preemption disabled -- a held plain
 * spinlock, which a target could be spinning on with IRQs off -- is refused (panicBug) before any
 * CPU runs `fn`. The lock and the preemptDisable are taken outside archTrapCatch(), so the catch
 * leaves nothing held (D-187). With only this CPU online the call runs locally, unrestricted. */
KTEST(smp_call_function_refused_with_lock_held) {
    CallState s = {0};
    if (smpOnlineCount() < 2) {
        preemptDisable();
        smpCallFunction(smpOnlineMask(), callCounter, &s);
        preemptEnable();
        KTEST_ASSERT_EQ(s.calls, 1);
        return;
    }
    TrapCatchInfo info;
    preemptDisable();
    bool caughtPreempt = archTrapCatch(TRAP_CATCH_KERNEL_BUG, callCountingTrigger, &s, &info);
    preemptEnable();
    spinLock(&callHeldLock);
    bool caughtLock = archTrapCatch(TRAP_CATCH_KERNEL_BUG, callCountingTrigger, &s, &info);
    spinUnlock(&callHeldLock);
    KTEST_ASSERT(caughtPreempt);
    KTEST_ASSERT(caughtLock);
    KTEST_ASSERT_EQ(ATOMIC_LOAD(&s.calls, MEM_SEQ_CST), 0);
    /* The same call with nothing held runs on every CPU. */
    smpCallFunction(smpOnlineMask(), callCounter, &s);
    KTEST_ASSERT_EQ(ATOMIC_LOAD(&s.calls, MEM_SEQ_CST), smpOnlineCount());
}

static uint64_t tlbHandled(uint32_t cpu) {
    return ATOMIC_LOAD(&cpuLocalOf(cpu)->mbox.handled[1], MEM_SEQ_CST);
}

/* One vmalloc area torn down is one shootdown round per CPU (batched), however many pages it has,
 * up to the 64-page chunk vfree() unmaps at a time; 65 pages is two chunks. */
KTEST(smp_tlb_shootdown_batched) {
    static const struct {
        uint32_t pages, rounds;
    } cases[] = {{1, 1}, {8, 1}, {33, 1}, {64, 1}, {65, 2}};
    uint32_t n = smpOnlineCount();
    for (uint32_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        void *p = vmalloc((size_t)cases[c].pages * 4096, 0);
        KTEST_ASSERT(p != NULL);
        uint64_t before[CPU_MAX];
        for (uint32_t cpu = 1; cpu < n; cpu++) {
            before[cpu] = tlbHandled(cpu);
        }
        vfree(p);
        for (uint32_t cpu = 1; cpu < n; cpu++) {
            KTEST_ASSERT_EQ(tlbHandled(cpu) - before[cpu], cases[c].rounds);
        }
    }
}

/* The stop IPI marks every other CPU stopped (here in test mode: they keep running afterwards). */
KTEST(smp_stop_parks_cpus) {
    smpStopTestMode(true);
    /* The IPI alone, with no NMI fallback behind it: smpStopOthers() below would NMI a CPU the IPI
     * failed to stop, and that fallback would hide a broken stop-IPI handler (found by the M3.5
     * bug sweep's mutation check). */
    for (uint32_t cpu = 1; cpu < smpOnlineCount(); cpu++) {
        archSmpSendIpi(cpu, SMP_VECTOR_STOP);
        uint64_t deadline = timeMonotonicNs() + 2000000000ull;
        while (!smpCpuStopped(cpu) && timeMonotonicNs() < deadline) {
            archPause();
        }
        bool stoppedByIpi = smpCpuStopped(cpu);
        if (!stoppedByIpi) {
            smpStopTestMode(false);
        }
        KTEST_ASSERT(stoppedByIpi);
    }
    smpStopTestMode(false); /* clears every `stopped` flag */
    smpStopTestMode(true);
    smpStopOthers();
    for (uint32_t cpu = 1; cpu < smpOnlineCount(); cpu++) {
        KTEST_ASSERT(smpCpuStopped(cpu));
    }
    KTEST_ASSERT(!smpCpuStopped(0));
    smpStopTestMode(false);
    for (uint32_t cpu = 0; cpu < smpOnlineCount(); cpu++) {
        KTEST_ASSERT(!smpCpuStopped(cpu));
    }
}

/* --- allocation from a handler (D-200) ---------------------------------------------------------
 */

static volatile uint32_t allocCbDone, allocCbOk;

static void allocInIrqCallback(TimerObj *t, void *ctx) {
    (void)t;
    (void)ctx;
    uint32_t ok = 1;
    void *small = kmalloc(64, KMALLOC_ZERO);
    void *big = kmalloc(4096, 0);
    Page *pg = NULL;
    if (small == NULL || big == NULL || pmmAllocPages(0, 0, &pg) != STATUS_OK) {
        ok = 0;
    } else {
        pmmFreePages(pg, 0);
    }
    kfree(small);
    kfree(big);
    allocCbOk = ok;
    allocCbDone = 1;
}

/* pmm, slab and kmalloc are usable from a hard-IRQ handler since M3.5 (D-200): their locks are
 * irqsave and the validate/claim steps run under them. vmalloc and the vmm map/unmap paths are not
 * (they may wait for a TLB shootdown). */
KTEST(irq_handler_may_allocate) {
    static TimerObj timer;
    allocCbDone = 0;
    allocCbOk = 0;
    timerInit(&timer, allocInIrqCallback, NULL);
    KTEST_ASSERT(timerArm(&timer, timeMonotonicNs()) == STATUS_OK);
    uint64_t deadline = timeMonotonicNs() + 1000000000ull;
    while (!allocCbDone && timeMonotonicNs() < deadline) {
        archPause();
    }
    KTEST_ASSERT(allocCbDone);
    KTEST_ASSERT_EQ(allocCbOk, 1);
}

/* --- call-function storm ------------------------------------------------------------------------
 */

static uint32_t stormCalls;

static void stormCounter(void *arg) {
    (void)arg;
    ATOMIC_FETCH_ADD(&stormCalls, 1, MEM_SEQ_CST);
}

#define STORM_ROUNDS 100u

static void stormFn(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < STORM_ROUNDS; i++) {
        smpCallFunction(smpOnlineMask(), stormCounter, NULL);
    }
}

/* Every CPU calls every CPU at the same time, over and over: A waits for B while B waits for A, and
 * both keep servicing IPIs because they wait with IF=1 (D-195). Nothing may deadlock and every call
 * must reach every CPU exactly once. */
KTEST(smp_call_function_storm) {
    stormCalls = 0;
    uint32_t n = smpOnlineCount();
    smpWorkRun(smpOnlineMask(), stormFn, NULL);
    KTEST_ASSERT_EQ(stormCalls, n * n * STORM_ROUNDS);
}

/* --- TLB shootdown stress (ROADMAP M3.5 item 7)
 * --------------------------------------------------- */

#define STRESS_PHASE_RUN   0u
#define STRESS_PHASE_PAUSE 1u
#define PAT_A              0xAAAAAAAAAAAAAAAAULL
#define PAT_B              0xBBBBBBBBBBBBBBBBULL

typedef struct {
    uint64_t va;
    uint32_t phase, stop;
    uint32_t gen;      /* even: the page maps frame A, odd: frame B */
    uint32_t pauseSeq; /* which pause the BSP is asking for */
    uint32_t acks;     /* acknowledgements so far, all readers and pauses */
    uint32_t reads;    /* verified reads, all readers */
    uint32_t bad;      /* reads that saw the wrong frame's pattern */
} Stress;

static Stress stress;

static void stressReader(void *arg) {
    Stress *s = arg;
    uint32_t ackedSeq = 0;
    while (!ATOMIC_LOAD(&s->stop, MEM_ACQUIRE)) {
        if (ATOMIC_LOAD(&s->phase, MEM_SEQ_CST) == STRESS_PHASE_PAUSE) {
            uint32_t seq = ATOMIC_LOAD(&s->pauseSeq, MEM_SEQ_CST);
            if (seq != ackedSeq) { /* stop touching the page, keeping whatever the TLB holds */
                ackedSeq = seq;
                ATOMIC_FETCH_ADD(&s->acks, 1, MEM_SEQ_CST);
            }
            archPause();
            continue;
        }
        uint32_t g = ATOMIC_LOAD(&s->gen, MEM_ACQUIRE);
        uint64_t want = (g & 1u) ? PAT_B : PAT_A;
        volatile const uint64_t *p = (volatile const uint64_t *)(uintptr_t)s->va;
        if (p[0] != want || p[511] != want) {
            ATOMIC_FETCH_ADD(&s->bad, 1, MEM_SEQ_CST);
        }
        ATOMIC_FETCH_ADD(&s->reads, 1, MEM_SEQ_CST);
    }
}

static Page *fillFrame(uint64_t pattern) {
    Page *pg;
    if (pmmAllocPages(0, 0, &pg) != STATUS_OK) {
        return NULL;
    }
    uint64_t *w = pmmPageToVirt(pg);
    for (uint32_t i = 0; i < 512; i++) {
        w[i] = pattern;
    }
    return pg;
}

/* One CPU flips a kernel mapping between two frames while the others read it. A reader pauses
 * (without touching the page, TLB entry intact) before each flip; after the flip it must see the
 * new frame. A shootdown that missed a CPU leaves that CPU reading the old frame. */
KTEST(smp_tlb_shootdown_stress) {
    uint32_t readers = smpOnlineCount() - 1;
    if (readers == 0) {
        return;
    }
    Page *a = fillFrame(PAT_A), *b = fillFrame(PAT_B);
    KTEST_ASSERT(a != NULL && b != NULL);
    uint64_t va;
    KTEST_ASSERT(vmmKvaAlloc(4096, &va) == STATUS_OK);
    KTEST_ASSERT(vmmMapKernel(va, pmmPageToPhys(a), 4096, VMM_WRITE) == STATUS_OK);

    stress = (Stress){0};
    stress.va = va;
    SmpWork w[CPU_MAX];
    for (uint32_t cpu = 1; cpu <= readers; cpu++) {
        w[cpu] = (SmpWork){.fn = stressReader, .arg = &stress, .remaining = 1};
        smpWorkPost(cpu, &w[cpu]);
    }
    const uint32_t flips = 400;
    bool ok = true;
    for (uint32_t it = 0; it < flips && ok; it++) {
        /* Let every reader verify at least one read of the current frame first. */
        uint32_t target = ATOMIC_LOAD(&stress.reads, MEM_SEQ_CST) + readers;
        uint64_t deadline = timeMonotonicNs() + 5000000000ull;
        while (ATOMIC_LOAD(&stress.reads, MEM_SEQ_CST) < target && timeMonotonicNs() < deadline) {
            archPause();
        }
        ATOMIC_STORE(&stress.pauseSeq, it + 1, MEM_SEQ_CST);
        ATOMIC_STORE(&stress.phase, STRESS_PHASE_PAUSE, MEM_SEQ_CST);
        deadline = timeMonotonicNs() + 5000000000ull;
        while (ATOMIC_LOAD(&stress.acks, MEM_SEQ_CST) < readers * (it + 1) &&
               timeMonotonicNs() < deadline) {
            archPause();
        }
        if (ATOMIC_LOAD(&stress.acks, MEM_SEQ_CST) < readers * (it + 1)) {
            klogWrite(KLOG_ERROR, "smp", "tlb-stress: flip %u: only %u of %u readers paused", it,
                      (unsigned)stress.acks, readers);
            ok = false;
            break;
        }
        Status us = vmmUnmapKernel(va, 4096);
        Page *next = (stress.gen & 1u) ? a : b;
        Status ms = vmmMapKernel(va, pmmPageToPhys(next), 4096, VMM_WRITE);
        ok = us == STATUS_OK && ms == STATUS_OK;
        if (!ok) {
            klogWrite(KLOG_ERROR, "smp", "tlb-stress: flip %u: unmap %d map %d", it, (int)us,
                      (int)ms);
        }
        ATOMIC_STORE(&stress.gen, stress.gen + 1, MEM_RELEASE);
        ATOMIC_STORE(&stress.phase, STRESS_PHASE_RUN, MEM_SEQ_CST);
    }
    uint32_t target = ATOMIC_LOAD(&stress.reads, MEM_SEQ_CST) + readers;
    uint64_t deadline = timeMonotonicNs() + 5000000000ull;
    while (ATOMIC_LOAD(&stress.reads, MEM_SEQ_CST) < target && timeMonotonicNs() < deadline) {
        archPause();
    }
    ATOMIC_STORE(&stress.stop, 1, MEM_RELEASE);
    for (uint32_t cpu = 1; cpu <= readers; cpu++) {
        smpWorkWait(&w[cpu]);
    }
    klogWrite(KLOG_INFO, "smp", "tlb-stress readers=%u flips=%u reads=%u bad=%u", readers, flips,
              stress.reads, stress.bad);
    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(stress.bad, 0);
    KTEST_ASSERT_EQ(stress.gen, flips);
    KTEST_ASSERT(vmmUnmapKernel(va, 4096) == STATUS_OK);
    vmmKvaFree(va, 4096);
    pmmFreePages(a, 0);
    pmmFreePages(b, 0);
}

/* --- concurrent pmm stress (ROADMAP M3.5 item 7) -------------------------------------------------
 */

#define PMM_STRESS_OPS  6000u
#define PMM_STRESS_LIVE 48u
#define PMM_STRESS_XCHG 64u

typedef struct {
    uint64_t seed;
    uint32_t bad;
    uint32_t ops;
} PmmStressCpu;

static PmmStressCpu pmmStress[CPU_MAX];
static Page *volatile pmmExchange[PMM_STRESS_XCHG];

static uint32_t rnd(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return (uint32_t)(x >> 11);
}

typedef struct {
    Page *page;
    uint32_t order;
} Live;

static void stampPage(Page *pg, uint64_t stamp) {
    *(uint64_t *)pmmPageToVirt(pg) = stamp;
}

static bool stampOk(Page *pg, uint64_t stamp) {
    return *(uint64_t *)pmmPageToVirt(pg) == stamp;
}

static void pmmStressFn(void *arg) {
    (void)arg;
    uint32_t cpu = smpThisCpu();
    PmmStressCpu *st = &pmmStress[cpu];
    st->seed = 0x9E3779B97F4A7C15ULL * (cpu + 1);
    Live live[PMM_STRESS_LIVE] = {0};
    for (uint32_t op = 0; op < PMM_STRESS_OPS; op++) {
        uint32_t slot = rnd(&st->seed) % PMM_STRESS_LIVE;
        if (live[slot].page != NULL) {
            Page *pg = live[slot].page;
            uint64_t stamp = ((uint64_t)cpu << 48) | op;
            (void)stamp;
            /* The stamp left in the page is (cpu, slot, order): check it survived. */
            if (!stampOk(pg, ((uint64_t)cpu << 48) | ((uint64_t)slot << 8) | live[slot].order)) {
                st->bad++;
            }
            if (live[slot].order == 0 && (rnd(&st->seed) & 3u) == 0) {
                /* A quarter of the order-0 frees go through another CPU: swap with the exchange
                 * array and free whatever was there (it belonged to some other CPU, which already
                 * checked nothing else owns it). */
                uint32_t x = rnd(&st->seed) % PMM_STRESS_XCHG;
                Page *other = __atomic_exchange_n(&pmmExchange[x], pg, __ATOMIC_ACQ_REL);
                if (other != NULL) {
                    pmmFreePages(other, 0);
                }
            } else {
                pmmFreePages(pg, live[slot].order);
            }
            live[slot].page = NULL;
        } else {
            uint32_t order = (rnd(&st->seed) & 7u) == 0 ? (rnd(&st->seed) % 4u) : 0u;
            Page *pg;
            if (pmmAllocPages(order, 0, &pg) != STATUS_OK) {
                continue;
            }
            stampPage(pg, ((uint64_t)cpu << 48) | ((uint64_t)slot << 8) | order);
            live[slot].page = pg;
            live[slot].order = order;
        }
        st->ops++;
    }
    for (uint32_t i = 0; i < PMM_STRESS_LIVE; i++) {
        if (live[i].page != NULL) {
            pmmFreePages(live[i].page, live[i].order);
        }
    }
}

/* Every CPU allocates and frees mixed-order blocks at once, a quarter of the single pages through
 * other CPUs' caches; afterwards, with every cache drained, the free count is back where it was and
 * no stamp was overwritten. */
KTEST(smp_pmm_concurrent_stress) {
    pmmDrainAllCaches();
    PmmStats before;
    pmmGetStats(&before);
    for (uint32_t i = 0; i < CPU_MAX; i++) {
        pmmStress[i] = (PmmStressCpu){0};
    }
    for (uint32_t i = 0; i < PMM_STRESS_XCHG; i++) {
        pmmExchange[i] = NULL;
    }
    smpWorkRun(smpOnlineMask(), pmmStressFn, NULL);
    for (uint32_t i = 0; i < PMM_STRESS_XCHG; i++) {
        if (pmmExchange[i] != NULL) {
            pmmFreePages(pmmExchange[i], 0);
            pmmExchange[i] = NULL;
        }
    }
    pmmDrainAllCaches();
    PmmStats after;
    pmmGetStats(&after);
    uint32_t bad = 0, ops = 0;
    for (uint32_t i = 0; i < smpOnlineCount(); i++) {
        bad += pmmStress[i].bad;
        ops += pmmStress[i].ops;
    }
    klogWrite(KLOG_INFO, "smp", "pmm-stress cpus=%u ops=%u bad=%u", smpOnlineCount(), ops, bad);
    KTEST_ASSERT_EQ(bad, 0);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
    KTEST_ASSERT_EQ(after.cachedPages, 0);
}

/* --- cross-CPU frees and double frees -----------------------------------------------------------
 */

static void *crossPtr;
static Page *crossPage;

static void crossFreeFn(void *arg) {
    (void)arg;
    kfree(crossPtr);
    pmmFreePages(crossPage, 0);
}

static void crossDoubleFreeTrigger(void *arg) {
    (void)arg;
    kfree(crossPtr);
}

static void crossPageDoubleFreeTrigger(void *arg) {
    (void)arg;
    pmmFreePages(crossPage, 0);
}

/* An object or frame freed on another CPU is parked in that CPU's cache; freeing it again here must
 * still be seen as a double free (D-199: the bufctl/Page state is global, not per cache). */
KTEST(smp_cross_cpu_double_free_caught) {
    if (smpOnlineCount() < 2) {
        return;
    }
    crossPtr = kmalloc(48, KMALLOC_ZERO);
    KTEST_ASSERT(crossPtr != NULL);
    KTEST_ASSERT(pmmAllocPages(0, 0, &crossPage) == STATUS_OK);
    SmpWork w = {.fn = crossFreeFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &w);
    smpWorkWait(&w);
    TrapCatchInfo info;
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, crossDoubleFreeTrigger, NULL, &info));
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, crossPageDoubleFreeTrigger, NULL, &info));
}

/* --- AP timers and the monotonic clock -----------------------------------------------------------
 */

typedef struct {
    TimerObj timer;
    volatile uint32_t fired;
    volatile uint32_t firedOn, firedDepth, armedOn;
} ApTimer;

static ApTimer apTimers[CPU_MAX];

static void apTimerCb(TimerObj *t, void *ctx) {
    (void)t;
    ApTimer *a = ctx;
    a->firedOn = smpThisCpu();
    a->firedDepth = irqDepth();
    a->fired = 1;
}

static void apTimerFn(void *arg) {
    (void)arg;
    ApTimer *a = &apTimers[smpThisCpu()];
    a->fired = 0;
    a->armedOn = smpThisCpu();
    timerInit(&a->timer, apTimerCb, a);
    if (timerArm(&a->timer, timeMonotonicNs() + 10000000ull) != STATUS_OK) {
        return;
    }
    uint64_t deadline = timeMonotonicNs() + 2000000000ull;
    while (!a->fired && timeMonotonicNs() < deadline) {
        archPause();
    }
    if (!a->fired) {
        (void)timerCancel(&a->timer);
    }
}

/* Every CPU's own LAPIC timer works: a timer armed on a CPU fires there, in IRQ context. */
KTEST(smp_ap_timer_fires_locally) {
    for (uint32_t i = 0; i < CPU_MAX; i++) {
        apTimers[i] = (ApTimer){0};
    }
    smpWorkRun(smpOnlineMask(), apTimerFn, NULL);
    for (uint32_t i = 0; i < smpOnlineCount(); i++) {
        KTEST_ASSERT_EQ(apTimers[i].fired, 1);
        KTEST_ASSERT_EQ(apTimers[i].firedOn, i);
        KTEST_ASSERT_EQ(apTimers[i].firedDepth, 1);
    }
}

typedef struct {
    uint64_t last;
    uint32_t turn;
    uint32_t bad;
    uint32_t rounds;
} TscChain;

static TscChain tscChain;

static void tscChainFn(void *arg) {
    TscChain *c = arg;
    uint32_t me = smpThisCpu(), n = smpOnlineCount();
    for (;;) {
        uint32_t turn = ATOMIC_LOAD(&c->turn, MEM_ACQUIRE);
        if (turn >= c->rounds * n) {
            return;
        }
        if (turn % n != me) {
            archPause();
            continue;
        }
        uint64_t now = timeMonotonicNs();
        if (now < c->last) {
            ATOMIC_FETCH_ADD(&c->bad, 1, MEM_SEQ_CST);
        }
        c->last = now;
        ATOMIC_STORE(&c->turn, turn + 1, MEM_RELEASE);
    }
}

/* timeMonotonicNs() never goes backwards across CPUs: the CPUs take turns reading it, each read
 * ordered after the previous one by the handoff, and every reading must be >= the last. */
KTEST(smp_tsc_monotonic) {
    tscChain = (TscChain){0};
    tscChain.rounds = 2000;
    smpWorkRun(smpOnlineMask(), tscChainFn, &tscChain);
    KTEST_ASSERT_EQ(tscChain.bad, 0);
    KTEST_ASSERT_EQ(tscChain.turn, tscChain.rounds * smpOnlineCount());
}
