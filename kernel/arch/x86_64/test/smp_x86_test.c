/* x86 ktests for SMP bring-up (M3.5, D-190..D-198): per-CPU tables and registers, the cross-CPU
 * TSC estimator and the stop NMI fallback. The work runs on the APs through smpWorkPost()/
 * smpWorkRun() (D-202); an AP's results travel through shared memory and the BSP asserts, since the
 * ktest state is the BSP's. */
#include "gdt.h"
#include "smp-impl.h"

#include "atomic.h"
#include "cpu-local.h"
#include "klog.h"
#include "ktest.h"
#include "smp.h"
#include "timekeeping.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stdint.h>

#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u
#define MSR_IA32_EFER      0xC0000080u
#define MSR_IA32_PAT       0x277u

typedef struct {
    uint32_t ran;
    uint32_t fail; /* bit i set: check i failed */
    uint64_t cr0, cr3, cr4, efer, pat, idtBase;
} CpuProbe;

static CpuProbe probes[CPU_MAX];

enum {
    FAIL_GS = 1u << 0,
    FAIL_SELF = 1u << 1,
    FAIL_KERNEL_GS = 1u << 2,
    FAIL_TR = 1u << 3,
    FAIL_GDTR = 1u << 4,
    FAIL_TSS_DESC = 1u << 5,
    FAIL_TSS_IST = 1u << 6,
    FAIL_STACK = 1u << 7,
    FAIL_ID = 1u << 8,
};

static void probeFn(void *arg) {
    (void)arg;
    CpuLocal *cl = cpuLocal();
    CpuProbe *p = &probes[cl->cpuId];
    uint32_t fail = 0;
    if (archRdmsr(MSR_GS_BASE) != (uint64_t)(uintptr_t)cl) {
        fail |= FAIL_GS;
    }
    if (cl->self != cl) {
        fail |= FAIL_SELF;
    }
    if (archRdmsr(MSR_KERNEL_GS_BASE) != 0) {
        fail |= FAIL_KERNEL_GS;
    }
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    if (tr != GDT_SEL_TSS) {
        fail |= FAIL_TR;
    }
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } gdtr, idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    if (gdtr.base != (uint64_t)(uintptr_t)cl->arch.gdt) {
        fail |= FAIL_GDTR;
    }
    /* The TSS descriptor (slots 6-7) names this CPU's own TSS. */
    uint64_t lo = cl->arch.gdt[6], hi = cl->arch.gdt[7];
    uint64_t base = ((lo >> 16) & 0xFFFFFFULL) | (((lo >> 56) & 0xFFULL) << 24) | (hi << 32);
    if (base != (uint64_t)(uintptr_t)&cl->arch.tss) {
        fail |= FAIL_TSS_DESC;
    }
    for (uint32_t i = 0; i < 3; i++) {
        if (cl->arch.tss.ist[i] != cl->arch.istTop[i]) {
            fail |= FAIL_TSS_IST;
        }
    }
    uint64_t rsp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    if (rsp < cl->arch.stackBottom || rsp >= cl->arch.stackTop) {
        fail |= FAIL_STACK;
    }
    uint32_t id = archCpuApicId();
    if (id != cl->apicId) {
        fail |= FAIL_ID;
    }
    p->cr0 = archReadCr0();
    p->cr3 = archReadCr3();
    p->cr4 = archReadCr4();
    p->efer = archRdmsr(MSR_IA32_EFER);
    p->pat = archRdmsr(MSR_IA32_PAT);
    p->idtBase = idtr.base;
    p->fail = fail;
    ATOMIC_STORE(&p->ran, 1, MEM_RELEASE);
}

static void runProbes(void) {
    for (uint32_t i = 0; i < CPU_MAX; i++) {
        probes[i] = (CpuProbe){0};
    }
    smpWorkRun(smpOnlineMask(), probeFn, NULL);
}

/* ROADMAP M3.5 item 1: every CPU has its own GS-based CpuLocal, GDT, TSS, IST stacks and kernel
 * stack, all of them checked from the CPU itself. */
KTEST(smp_gs_tables_per_cpu) {
    runProbes();
    uint32_t n = smpOnlineCount();
    for (uint32_t i = 0; i < n; i++) {
        KTEST_ASSERT_EQ(probes[i].ran, 1);
        KTEST_ASSERT_EQ(probes[i].fail, 0);
        CpuLocal *a = cpuLocalOf(i);
        for (uint32_t j = 0; j < i; j++) {
            CpuLocal *b = cpuLocalOf(j);
            KTEST_ASSERT(a != b);
            KTEST_ASSERT(a->arch.stackBottom != b->arch.stackBottom);
            for (uint32_t k = 0; k < 3; k++) {
                for (uint32_t m = 0; m < 3; m++) {
                    KTEST_ASSERT(a->arch.istTop[k] != b->arch.istTop[m]);
                }
                KTEST_ASSERT(a->arch.istTop[k] != b->arch.stackTop);
            }
        }
        for (uint32_t k = 0; k < 3; k++) { /* distinct stacks per CPU, 16 KiB each */
            KTEST_ASSERT_EQ(a->arch.istTop[k] - a->arch.istBottom[k], 0x4000);
        }
    }
}

/* Every AP reproduces the boot CPU's CR0/CR3/CR4/EFER/PAT and shares its IDT (D-194). */
KTEST(smp_cpu_state_matches_bsp) {
    runProbes();
    uint32_t n = smpOnlineCount();
    for (uint32_t i = 1; i < n; i++) {
        KTEST_ASSERT_EQ(probes[i].cr0, probes[0].cr0);
        KTEST_ASSERT_EQ(probes[i].cr3, probes[0].cr3);
        KTEST_ASSERT_EQ(probes[i].cr4, probes[0].cr4);
        KTEST_ASSERT_EQ(probes[i].efer, probes[0].efer);
        KTEST_ASSERT_EQ(probes[i].pat, probes[0].pat);
        KTEST_ASSERT_EQ(probes[i].idtBase, probes[0].idtBase);
    }
}

/* The TSC estimator recovers a known skew (D-198): an AP whose readings are `skew` ticks ahead (or
 * behind) shows warps, the estimate lands within a round trip's worth of it, and applying it
 * removes nearly all of them. With no skew there is nothing to find. */
KTEST(smp_tsc_estimator_recovers_skew) {
    if (smpOnlineCount() < 2) {
        return;
    }
    static const int64_t skews[] = {0, 50000000, -50000000};
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t before = 0, after = 0;
        int64_t est = 0;
        KTEST_ASSERT(archTscSyncTest(1, skews[i], &before, &after, &est));
        if (skews[i] == 0) {
            KTEST_ASSERT_EQ(before, 0);
            KTEST_ASSERT_EQ(est, 0);
            KTEST_ASSERT_EQ(after, 0);
        } else {
            KTEST_ASSERT(before > 0);
            int64_t err = est - skews[i];
            if (err < 0) {
                err = -err;
            }
            KTEST_ASSERT(err < 2000000);
            /* The estimate is only as good as the round trip (half of it), so warps remain where
             * consecutive stamps are closer than that error (how many varies a lot between runs and
             * profiles: 0 to about a third of `before`); applying it must still reduce them. */
            KTEST_ASSERT(after < before);
        }
    }
}

typedef struct {
    uint64_t ticks;            /* the pretend skew, in TSC ticks */
    uint64_t before, during;   /* timeMonotonicNs() without and with it */
    uint32_t ran, notRestored; /* per CPU */
} OffsetProbe;

static OffsetProbe offsetProbes[CPU_MAX];

static void offsetProbeFn(void *arg) {
    (void)arg;
    CpuLocal *cl = cpuLocal();
    OffsetProbe *p = &offsetProbes[cl->cpuId];
    uint64_t f = archIrqSave(); /* no timer interrupt on this CPU sees the pretend offset */
    int64_t saved = cl->arch.tscOffset;
    p->before = timeMonotonicNs();
    cl->arch.tscOffset = saved + (int64_t)p->ticks; /* as if this TSC ran `ticks` ahead */
    p->during = timeMonotonicNs();
    cl->arch.tscOffset = saved;
    p->notRestored = timeMonotonicNs() < p->before;
    archIrqRestore(f);
    ATOMIC_STORE(&p->ran, 1, MEM_RELEASE);
}

/* D-198: timeMonotonicNs() subtracts the calling CPU's own tscOffset, so a CPU whose TSC runs ahead
 * by the estimated offset reads the same clock as the boot CPU. (Under QEMU the measured offset is
 * 0, so only a pretend one shows the subtraction is wired in, on every CPU.) */
KTEST(smp_tsc_offset_is_subtracted) {
    uint64_t ticks = timeTscHz() / 10; /* 100 ms */
    KTEST_ASSERT(ticks != 0);
    while (timeMonotonicNs() < 200000000ull) { /* so the pretend offset cannot wrap the clock */
        archPause();
    }
    for (uint32_t i = 0; i < CPU_MAX; i++) {
        offsetProbes[i] = (OffsetProbe){.ticks = ticks};
    }
    smpWorkRun(smpOnlineMask(), offsetProbeFn, NULL);
    for (uint32_t i = 0; i < smpOnlineCount(); i++) {
        const OffsetProbe *p = &offsetProbes[i];
        KTEST_ASSERT_EQ(p->ran, 1);
        KTEST_ASSERT(p->during < p->before);
        uint64_t back = p->before - p->during; /* 100 ms less the time between the two reads */
        KTEST_ASSERT(back > 50000000ull && back <= 100000000ull);
        KTEST_ASSERT_EQ(p->notRestored, 0);
    }
}

typedef struct {
    volatile uint32_t spinning, release;
} NmiSpin;

static void nmiSpinFn(void *arg) {
    NmiSpin *s = arg;
    archDisableInterrupts();
    ATOMIC_STORE(&s->spinning, 1, MEM_SEQ_CST);
    while (!ATOMIC_LOAD(&s->release, MEM_ACQUIRE)) {
        archPause();
    }
    archEnableInterrupts(); /* the stop IPI that was pending is delivered here */
}

/* A CPU that cannot take the stop IPI (IF=0) is stopped by the NMI fallback after 100 ms (D-197).
 */
KTEST(smp_stop_nmi_fallback) {
    if (smpOnlineCount() < 2) {
        return;
    }
    static NmiSpin spin;
    spin.spinning = 0;
    spin.release = 0;
    SmpWork w = {.fn = nmiSpinFn, .arg = &spin, .remaining = 1};
    smpWorkPost(1, &w);
    uint64_t deadline = timeMonotonicNs() + 2000000000ull;
    while (!ATOMIC_LOAD(&spin.spinning, MEM_ACQUIRE) && timeMonotonicNs() < deadline) {
        archPause();
    }
    KTEST_ASSERT(spin.spinning);
    uint64_t stopIpis = cpuLocalOf(1)->mbox.ipiCount[3];
    smpStopTestMode(true);
    smpStopOthers();
    bool stopped = smpCpuStopped(1); /* only the NMI could have done it: IF=0 on cpu 1 */
    bool pendingStop = ATOMIC_LOAD(&cpuLocalOf(1)->mbox.ipiCount[3], MEM_ACQUIRE) == stopIpis;
    ATOMIC_STORE(&spin.release, 1, MEM_RELEASE);
    smpWorkWait(&w);
    /* The pending stop IPI lands once cpu 1 re-enables interrupts: keep the test mode on until then
     * (otherwise it would park the CPU for good). */
    deadline = timeMonotonicNs() + 2000000000ull;
    while (cpuLocalOf(1)->mbox.ipiCount[3] == stopIpis && timeMonotonicNs() < deadline) {
        archPause();
    }
    bool delivered = cpuLocalOf(1)->mbox.ipiCount[3] != stopIpis;
    smpStopTestMode(false);
    KTEST_ASSERT(stopped);
    KTEST_ASSERT(pendingStop);
    KTEST_ASSERT(delivered);
}
