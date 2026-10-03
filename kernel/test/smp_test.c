/* ktests for per-CPU data and SMP bring-up (M3.5, D-190..). */
#include "acpi.h"
#include "atomic.h"
#include "cpu-local.h"
#include "kernel-boot.h"
#include "ktest.h"
#include "preempt.h"
#include "smp.h"
#include "vmalloc.h"

#include <arch/cpu.h>
#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

#define MSR_GS_BASE        0xC0000101U
#define MSR_KERNEL_GS_BASE 0xC0000102U

/* D-190: the BSP's GS base is its CpuLocal, `self` is at offset 0, the TSS and GDT it runs on are
 * the ones in its own CpuLocal, and cpuSync()/irqDepth live in it. */
KTEST(smp_cpulocal_bsp) {
    CpuLocal *cl = cpuLocal();
    KTEST_ASSERT(cl == &cpuLocalBsp);
    KTEST_ASSERT(cl->self == cl);
    KTEST_ASSERT_EQ(archRdmsr(MSR_GS_BASE), (uint64_t)(uintptr_t)cl);
    KTEST_ASSERT_EQ(archRdmsr(MSR_KERNEL_GS_BASE), 0);
    KTEST_ASSERT_EQ(cl->cpuId, 0);
    KTEST_ASSERT(cpuLocalOf(0) == cl);
    KTEST_ASSERT(cpuLocalOf(CPU_MAX) == NULL);
    KTEST_ASSERT(cpuSync() == &cl->sync);

    uint64_t gdtrBuf[2] = {0, 0};
    __asm__ volatile("sgdt %0" : "=m"(gdtrBuf));
    KTEST_ASSERT_EQ((gdtrBuf[0] >> 16) | (gdtrBuf[1] << 48), (uint64_t)(uintptr_t)cl->arch.gdt);
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    KTEST_ASSERT_EQ(tr, 0x30);
    uint64_t rsp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    KTEST_ASSERT(rsp >= cl->arch.stackBottom && rsp < cl->arch.stackTop);
}

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
