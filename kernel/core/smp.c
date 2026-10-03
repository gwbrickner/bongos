/* The portable half of SMP state: the online mask (D-190). See kernel/include/smp.h. */
#include "smp.h"

#include "atomic.h"
#include "cpu-local.h"
#include "irq.h"
#include "ktest.h"
#include "panic.h"
#include "preempt.h"
#include "timekeeping.h"

#include <arch/cpu.h>

static uint64_t onlineMask = 1; /* the BSP (dense id 0) is online from the start */

uint64_t smpOnlineMask(void) {
    return ATOMIC_LOAD(&onlineMask, MEM_ACQUIRE);
}

uint32_t smpOnlineCount(void) {
    return (uint32_t)__builtin_popcountll(smpOnlineMask());
}

void smpMarkOnline(uint32_t cpuId) {
    ATOMIC_FETCH_OR(&onlineMask, (uint64_t)1 << cpuId, MEM_SEQ_CST);
}

/* --- cross-CPU calls (D-195)
 * ---------------------------------------------------------------------- */

#define SMP_KIND_CALL 0u
#define SMP_KIND_TLB  1u

#define SMP_ACK_TIMEOUT_NS  10000000000ull /* a CPU that does not answer in 10 s is dead */
#define SMP_STOP_TIMEOUT_NS 100000000ull   /* 100 ms for the stop IPI before the NMI */

struct SmpReq {
    SmpFn fn;
    void *arg;
    uint32_t kind;
    uint32_t remaining; /* CPUs still to run `fn`; the sender waits for 0 */
};

void smpCallFunctionVec(uint64_t cpuMask, SmpFn fn, void *arg, uint32_t vector) {
    if (fn == NULL || (vector != SMP_VECTOR_TLB && vector != SMP_VECTOR_CALL)) {
        panicBug("smpCallFunction: NULL function or bad vector");
    }
    uint32_t me = smpThisCpu();
    uint64_t mask = cpuMask & smpOnlineMask();
    uint64_t others = mask & ~((uint64_t)1 << me);

    if (others == 0) {
        if (mask != 0) { /* only this CPU: local, with IRQs off like a handler */
            uint64_t f = archIrqSave();
            fn(arg);
            archIrqRestore(f);
        }
        return;
    }
    if (!archInterruptsEnabled() || irqDepth() != 0) {
        panicBug("smpCallFunction: needs IF=1 and no handler running (a wait with IRQs off can "
                 "deadlock against another CPU's call)");
    }

    preemptDisable(); /* one outstanding request per CPU: do not migrate or nest another send */
    struct SmpReq req = {
        .fn = fn,
        .arg = arg,
        .kind = vector == SMP_VECTOR_TLB ? SMP_KIND_TLB : SMP_KIND_CALL,
        .remaining = (uint32_t)__builtin_popcountll(others),
    };
    for (uint64_t m = others; m != 0; m &= m - 1) {
        uint32_t t = (uint32_t)__builtin_ctzll(m);
        CpuLocal *tcl = cpuLocalOf(t);
        ATOMIC_STORE(&tcl->mbox.slot[me], &req, MEM_RELEASE);
        ATOMIC_FETCH_OR(&tcl->mbox.pending, (uint64_t)1 << me, MEM_SEQ_CST);
        archSmpSendIpi(t, vector);
    }
    if (mask & ((uint64_t)1 << me)) {
        uint64_t f = archIrqSave();
        fn(arg);
        archIrqRestore(f);
    }
    uint64_t deadline = timeMonotonicNs() + SMP_ACK_TIMEOUT_NS;
    while (ATOMIC_LOAD(&req.remaining, MEM_ACQUIRE) != 0) {
        if (timeMonotonicNs() >= deadline) {
            panic("smp: cpus did not answer a call-function IPI in 10 s (%u still pending)",
                  (unsigned)ATOMIC_LOAD(&req.remaining, MEM_RELAXED));
        }
        archPause();
    }
    preemptEnable();
}

void smpCallFunction(uint64_t cpuMask, SmpFn fn, void *arg) {
    smpCallFunctionVec(cpuMask, fn, arg, SMP_VECTOR_CALL);
}

void smpKick(uint32_t cpuId) {
    archSmpSendIpi(cpuId, SMP_VECTOR_KICK);
}

/* Runs every request posted to this CPU. Called from the 0xF0/0xF2 handlers (IF=0, irqDepth()==1).
 */
static void smpDrainMailbox(CpuLocal *cl) {
    uint64_t m = ATOMIC_XCHG(&cl->mbox.pending, 0, MEM_ACQ_REL);
    for (; m != 0; m &= m - 1) {
        uint32_t sender = (uint32_t)__builtin_ctzll(m);
        struct SmpReq *r = ATOMIC_XCHG(&cl->mbox.slot[sender], NULL, MEM_ACQUIRE);
        if (r == NULL) {
            continue;
        }
        uint32_t kind = r->kind;
        r->fn(r->arg);
        ATOMIC_FETCH_ADD(&cl->mbox.handled[kind], 1, MEM_RELAXED);
        ATOMIC_FETCH_SUB(&r->remaining, 1, MEM_RELEASE); /* `r` may vanish the moment this lands */
    }
}

static bool stopTestMode;

void smpStopTestMode(bool on) {
    if (!ktestIsActive()) {
        panicBug("smpStopTestMode: ktest only");
    }
    if (!on) {
        for (uint32_t i = 0; i < CPU_MAX; i++) {
            CpuLocal *cl = cpuLocalOf(i);
            if (cl != NULL) {
                ATOMIC_STORE(&cl->stopped, 0, MEM_RELEASE);
            }
        }
    }
    ATOMIC_STORE(&stopTestMode, on, MEM_SEQ_CST);
}

bool smpCpuStopped(uint32_t cpuId) {
    CpuLocal *cl = cpuLocalOf(cpuId);
    return cl != NULL && ATOMIC_LOAD(&cl->stopped, MEM_ACQUIRE) != 0;
}

_Noreturn void smpParkSelf(void) {
    archDisableInterrupts();
    CpuLocal *cl = cpuLocal();
    ATOMIC_STORE(&cl->stopped, 1, MEM_RELEASE);
    archHaltForever();
}

void smpStopOthers(void) {
    uint32_t me = smpThisCpu();
    uint64_t others = smpOnlineMask() & ~((uint64_t)1 << me);
    for (uint64_t m = others; m != 0; m &= m - 1) {
        archSmpSendIpi((uint32_t)__builtin_ctzll(m), SMP_VECTOR_STOP);
    }
    uint64_t deadline = timeMonotonicNs() + SMP_STOP_TIMEOUT_NS;
    uint64_t pending = others;
    while (pending != 0 && timeMonotonicNs() < deadline) {
        for (uint64_t m = pending; m != 0; m &= m - 1) {
            uint32_t t = (uint32_t)__builtin_ctzll(m);
            if (smpCpuStopped(t)) {
                pending &= ~((uint64_t)1 << t);
            }
        }
        archPause();
    }
    /* A CPU that never took the IPI (IF=0 in a spin) gets an NMI, which no IF=0 stops. */
    for (uint64_t m = pending; m != 0; m &= m - 1) {
        archSmpSendNmi((uint32_t)__builtin_ctzll(m));
    }
    deadline = timeMonotonicNs() + SMP_STOP_TIMEOUT_NS;
    while (pending != 0 && timeMonotonicNs() < deadline) {
        for (uint64_t m = pending; m != 0; m &= m - 1) {
            uint32_t t = (uint32_t)__builtin_ctzll(m);
            if (smpCpuStopped(t)) {
                pending &= ~((uint64_t)1 << t);
            }
        }
        archPause();
    }
}

/* The IPI handler for all four vectors (registered by smpInit(), D-195). */
void smpIpiHandler(uint32_t vector, void *ctx) {
    (void)ctx;
    CpuLocal *cl = cpuLocal();
    ATOMIC_FETCH_ADD(&cl->mbox.ipiCount[vector - SMP_VECTOR_TLB], 1, MEM_RELAXED);
    switch (vector) {
        case SMP_VECTOR_TLB:
        case SMP_VECTOR_CALL:
            smpDrainMailbox(cl);
            break;
        case SMP_VECTOR_STOP:
            ATOMIC_STORE(&cl->stopped, 1, MEM_RELEASE);
            if (!ATOMIC_LOAD(&stopTestMode, MEM_ACQUIRE)) {
                archHaltForever(); /* never EOIs: nothing more is delivered here */
            }
            break;
        default: /* SMP_VECTOR_KICK: waking from hlt is all it is for */
            break;
    }
}

/* --- work for idle APs (D-202) ------------------------------------------------------------------
 */

void smpWorkPost(uint32_t cpuId, SmpWork *w) {
    CpuLocal *tcl = cpuLocalOf(cpuId);
    if (tcl == NULL || cpuId == smpThisCpu()) {
        panicBug("smpWorkPost: cpu %u is offline or the caller", cpuId);
    }
    SmpWork *expected = NULL;
    if (!ATOMIC_CMPXCHG(&tcl->work, &expected, w, MEM_RELEASE, MEM_RELAXED)) {
        panicBug("smpWorkPost: cpu %u still has earlier work pending", cpuId);
    }
    smpKick(cpuId);
}

void smpWorkWait(SmpWork *w) {
    if (!archInterruptsEnabled() || irqDepth() != 0) {
        panicBug("smpWorkWait: needs IF=1 and no handler running");
    }
    uint64_t deadline = timeMonotonicNs() + 30000000000ull;
    while (ATOMIC_LOAD(&w->remaining, MEM_ACQUIRE) != 0) {
        if (timeMonotonicNs() >= deadline) {
            panic("smp: posted work did not finish in 30 s (%u cpus pending)",
                  (unsigned)ATOMIC_LOAD(&w->remaining, MEM_RELAXED));
        }
        archPause();
    }
}

void smpWorkRun(uint64_t cpuMask, SmpFn fn, void *arg) {
    uint32_t me = smpThisCpu();
    uint64_t mask = cpuMask & smpOnlineMask();
    uint64_t others = mask & ~((uint64_t)1 << me);
    SmpWork w = {.fn = fn, .arg = arg, .remaining = (uint32_t)__builtin_popcountll(others)};
    for (uint64_t m = others; m != 0; m &= m - 1) {
        smpWorkPost((uint32_t)__builtin_ctzll(m), &w);
    }
    if (mask & ((uint64_t)1 << me)) {
        fn(arg);
    }
    if (others != 0) {
        smpWorkWait(&w);
    }
}

bool smpWorkRunPending(void) {
    CpuLocal *cl = cpuLocal();
    SmpWork *w = ATOMIC_XCHG(&cl->work, NULL, MEM_ACQUIRE);
    if (w == NULL) {
        return false;
    }
    archEnableInterrupts();
    SmpFn fn = w->fn;
    void *arg = w->arg;
    fn(arg);
    archDisableInterrupts();
    ATOMIC_FETCH_SUB(&w->remaining, 1, MEM_RELEASE); /* `w` may vanish the moment this lands */
    return true;
}

bool smpStopNmiHook(void) {
    if (!ATOMIC_LOAD(&stopTestMode, MEM_ACQUIRE)) {
        return false;
    }
    ATOMIC_STORE(&cpuLocal()->stopped, 1, MEM_RELEASE);
    return true;
}
