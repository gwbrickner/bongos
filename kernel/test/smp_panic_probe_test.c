/* Run-ending SMP panic probes (D-197, D-201), driven by tests/harness/smp-panic-check.sh (make
 * test-full). Each one panics on purpose, so each is a no-op (and passes) unless the command line
 * also carries the `smp-panic-probe` token: under ktest=all they never fire. The harness boots one
 * image per probe and checks that the run ends FAIL (not HANG or CRASH) with exactly one panic
 * report, from the expected CPU path, with no "PANIC while already panicking". */
#include "atomic.h"
#include "cmdline.h"
#include "cpu-local.h"
#include "kernel-boot.h"
#include "klog.h"
#include "ktest.h"
#include "lockdep.h"
#include "panic.h"
#include "smp.h"
#include "spinlock.h"

#include <arch/cpu.h>

static bool probeArmed(void) {
    return cmdlineHasToken(kernelCmdline(), "smp-panic-probe") && smpOnlineCount() >= 2;
}

static void probeApPanicFn(void *arg) {
    (void)arg;
    panic("smp-panic-probe: cpu %u panics", smpThisCpu());
}

/* An AP panics while the BSP waits for it with IF=1 (the stop IPI parks the BSP). */
KTEST(smp_panic_probe_ap) {
    if (!probeArmed()) {
        return;
    }
    smpWorkRun(2, probeApPanicFn, NULL);
    KTEST_ASSERT(false); /* not reached: the panic ends the run */
}

static volatile uint32_t probeGate;

static void probeAllPanicFn(void *arg) {
    (void)arg;
    ATOMIC_FETCH_ADD(&probeGate, 1, MEM_SEQ_CST);
    while (ATOMIC_LOAD(&probeGate, MEM_SEQ_CST) < smpOnlineCount()) {
        archPause();
    }
    panic("smp-panic-probe: cpu %u panics with every other cpu", smpThisCpu());
}

/* Every CPU panics at the same moment: exactly one report, the losers park silently. */
KTEST(smp_panic_probe_all) {
    if (!probeArmed()) {
        return;
    }
    probeGate = 0;
    smpWorkRun(smpOnlineMask(), probeAllPanicFn, NULL);
    KTEST_ASSERT(false);
}

static volatile uint32_t probeSpinners;

static void probeSpinIrqOffFn(void *arg) {
    (void)arg;
    archDisableInterrupts();
    ATOMIC_FETCH_ADD(&probeSpinners, 1, MEM_SEQ_CST);
    for (;;) {
        archPause();
    }
}

/* The BSP panics while every AP spins with IF=0: only the NMI fallback can stop them. */
KTEST(smp_panic_probe_bsp_aps_irqoff) {
    (void)ktestCtx;
    if (!probeArmed()) {
        return;
    }
    static SmpWork w[CPU_MAX];
    uint32_t n = smpOnlineCount();
    probeSpinners = 0;
    for (uint32_t c = 1; c < n; c++) {
        w[c] = (SmpWork){.fn = probeSpinIrqOffFn, .arg = NULL, .remaining = 1};
        smpWorkPost(c, &w[c]);
    }
    while (ATOMIC_LOAD(&probeSpinners, MEM_SEQ_CST) < n - 1) {
        archPause();
    }
    panic("smp-panic-probe: the boot cpu panics while %u cpus spin with IF=0", n - 1);
}

static volatile uint32_t probeLogLines;

static void probeLogFn(void *arg) {
    (void)arg;
    for (uint32_t i = 0;; i++) {
        klogWrite(KLOG_INFO, "smp", "probe-spam cpu %u line %u", smpThisCpu(), i);
        ATOMIC_FETCH_ADD(&probeLogLines, 1, MEM_SEQ_CST);
    }
}

/* The BSP panics while every AP keeps logging with IF=1: the stop IPI must silence them before the
 * report is printed (D-197), so no AP line may follow the PANIC line (the harness checks). */
KTEST(smp_panic_probe_bsp_aps_logging) {
    (void)ktestCtx;
    if (!probeArmed()) {
        return;
    }
    static SmpWork w[CPU_MAX];
    uint32_t n = smpOnlineCount();
    probeLogLines = 0;
    for (uint32_t c = 1; c < n; c++) {
        w[c] = (SmpWork){.fn = probeLogFn, .arg = NULL, .remaining = 1};
        smpWorkPost(c, &w[c]);
    }
    while (ATOMIC_LOAD(&probeLogLines, MEM_SEQ_CST) < 8u * (n - 1)) {
        archPause();
    }
    panic("smp-panic-probe: the boot cpu panics while %u cpus log", n - 1);
}

#ifdef KERNEL_DEBUG
static Spinlock probeLockA = SPINLOCK_INIT("probe-inv-a");
static Spinlock probeLockB = SPINLOCK_INIT("probe-inv-b");

static void probeApInversionFn(void *arg) {
    (void)arg;
    uint64_t fa = spinLockIrqSave(&probeLockA);
    uint64_t fb = spinLockIrqSave(&probeLockB);
    spinUnlockIrqRestore(&probeLockB, fb);
    spinUnlockIrqRestore(&probeLockA, fa);
    fb = spinLockIrqSave(&probeLockB);
    fa = spinLockIrqSave(&probeLockA);
    spinUnlockIrqRestore(&probeLockA, fa);
    spinUnlockIrqRestore(&probeLockB, fb);
}

/* The BSP arms the lock validator's expect mode and cpu 1 commits an inversion of that very kind:
 * the report is not the BSP's, so it must panic as an unexpected report (D-201). */
KTEST(lockdep_probe_expect_other_cpu) {
    if (!probeArmed()) {
        return;
    }
    lockdepExpectBegin(LOCKDEP_REPORT_INVERSION);
    smpWorkRun(2, probeApInversionFn, NULL);
    (void)lockdepExpectEnd();
    KTEST_ASSERT(false); /* reached only if cpu 1's report was swallowed */
}
#endif
