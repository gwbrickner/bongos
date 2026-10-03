/* See lockdep.h. The kernel glue around lockdep-core.c: the graph and its raw lock, the IRQ and
 * context state, stack capture and the reports. Debug builds only. */
#include "lockdep.h"

#include "preempt.h"

#ifdef KERNEL_DEBUG

#include "atomic.h"
#include "backtrace.h"
#include "format.h"
#include "irq.h"
#include "klog.h"
#include "ktest.h"
#include "panic.h"
#include "sections.h"
#include "spinlock-raw.h"

#include <arch/cpu.h>
#include <stddef.h>

/* All of this is guarded by graphLock with IRQs off. An all-zero graph is valid, so the validator
 * works from the first klogWrite in kernelMain. graphLock is a RawSpinlock: it must not recurse
 * into the validator, and it is innermost: nothing but klog output (validation skipped, see
 * lockdepRecursion) is ever taken under it. */
static LockdepGraph graph;
static RawSpinlock graphLock = RAW_SPINLOCK_INIT;
static bool lockdepOff;
static struct {
    bool armed;
    uint32_t cpu; /* the arming CPU: another CPU's report of the same kind still panics (D-201) */
    LockdepVerdict kind;
    uint32_t count;
    uint32_t totalExpected;
} expect;

static __attribute__((noinline)) void captureTrace(LockdepTrace *dst) {
    dst->count = (uint32_t)backtraceCapture(archFramePointer(), dst->pc, LOCKDEP_TRACE_DEPTH);
}

static const char *kindName(LockdepVerdict k) {
    switch (k) {
        case LOCKDEP_REPORT_INVERSION:
            return "lock order inversion";
        case LOCKDEP_REPORT_RECURSION:
            return "recursive locking";
        case LOCKDEP_REPORT_IRQ_INCONSISTENT:
            return "inconsistent IRQ lock state";
        case LOCKDEP_REPORT_NOT_HELD:
            return "unlock of a lock not held";
        case LOCKDEP_REPORT_HELD_OVERFLOW:
            return "too many locks held";
        case LOCKDEP_REPORT_BAD_INIT:
            return "bad lock init";
        case LOCKDEP_REPORT_IRQ_SAFE_UNSAFE:
            return "IRQ-safe lock reaches an IRQ-unsafe lock";
        default:
            return "?";
    }
}

static const char *usageName(uint8_t u) {
    return u == LOCKDEP_USAGE_IN_HARDIRQ ? "taken in a hard IRQ handler"
                                         : "taken with interrupts enabled";
}

static const char *className(uint16_t id) {
    if (id == 0 || id > graph.classCount) {
        return "?";
    }
    return graph.classes[id - 1u].name;
}

static void printTrace(const LockdepTrace *t) {
    if (t->count == 0) {
        klogRaw("  (no stack recorded)\n");
        return;
    }
    backtracePrintAddrs(t->pc, t->count);
}

static void outLine(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    klogRaw(line);
}

static void printHeld(const LockdepHeldStack *hs) {
    outLine("  held locks on this CPU: %u\n", hs->depth);
    for (uint32_t i = 0; i < hs->depth; i++) {
        const LockdepHeld *h = &hs->held[i];
        outLine("    \"%s\" (class %u) taken in %s context%s at:\n", className(h->classId),
                (unsigned)h->classId, h->irqCtx ? "IRQ" : "process",
                h->trylock ? " (trylock)" : "");
        backtracePrintAddrs(&h->ip, 1);
    }
}

/* Prints the report for `f` and either panics or, if a ktest expected this kind, returns. Called
 * with graphLock held, IRQs off and lockdepRecursion set, so its klog output (which takes
 * klogLock) is not validated. Uses panic(), never panicBug(): a longjmp out of here would strand
 * graphLock and lockdepRecursion. */
static void report(const LockdepFinding *f, const LockdepHeldStack *hs) {
    /* Re-taking the very same lock would spin forever once the report returns, so that one is never
     * swallowed. */
    bool expected = expect.armed && expect.kind == f->kind && expect.cpu == smpThisCpu() &&
                    !(f->kind == LOCKDEP_REPORT_RECURSION && f->sameInstance);
    outLine("%s: %s\n", expected ? "LOCKDEP (expected by ktest)" : "LOCKDEP", kindName(f->kind));
    switch (f->kind) {
        case LOCKDEP_REPORT_INVERSION:
            outLine("  acquiring \"%s\" (class %u) while holding \"%s\" (class %u)\n",
                    className(f->newClass), (unsigned)f->newClass, className(f->heldClass),
                    (unsigned)f->heldClass);
            outLine("  the existing dependency chain%s:\n",
                    f->pathTruncated ? " (its start is cut off)" : "");
            for (uint32_t i = 0; i < f->pathLen; i++) {
                uint16_t from = f->pathClass[i], to = f->pathClass[i + 1u];
                outLine("  \"%s\" -> \"%s\" first recorded at:\n", className(from), className(to));
                int e = lockdepCoreEdgeFind(&graph, from, to);
                if (e >= 0) {
                    printTrace(&graph.edges[e].trace);
                }
            }
            break;
        case LOCKDEP_REPORT_RECURSION:
            outLine("  \"%s\" (class %u) acquired while %s already held\n", className(f->newClass),
                    (unsigned)f->newClass,
                    f->sameInstance ? "this very lock is" : "another lock of its class is");
            break;
        case LOCKDEP_REPORT_IRQ_INCONSISTENT:
            outLine("  \"%s\" (class %u) is now %s,\n", className(f->newClass),
                    (unsigned)f->newClass, usageName(f->usageNew));
            outLine("  but it was earlier %s, at:\n", usageName(f->usageOld));
            printTrace(&graph.classes[f->newClass - 1u].usageTrace[f->usageOld]);
            break;
        case LOCKDEP_REPORT_IRQ_SAFE_UNSAFE:
            outLine("  \"%s\" (class %u) is %s, and through held-while-acquiring edges it\n",
                    className(f->safeClass), (unsigned)f->safeClass,
                    usageName(LOCKDEP_USAGE_IN_HARDIRQ));
            outLine("  reaches \"%s\" (class %u), which is %s; acquiring \"%s\" now (class %u)\n",
                    className(f->unsafeClass), (unsigned)f->unsafeClass,
                    usageName(LOCKDEP_USAGE_IRQS_ON), className(f->newClass),
                    (unsigned)f->newClass);
            outLine("  \"%s\" first %s at:\n", className(f->safeClass),
                    usageName(LOCKDEP_USAGE_IN_HARDIRQ));
            printTrace(&graph.classes[f->safeClass - 1u].usageTrace[LOCKDEP_USAGE_IN_HARDIRQ]);
            outLine("  \"%s\" first %s at:\n", className(f->unsafeClass),
                    usageName(LOCKDEP_USAGE_IRQS_ON));
            printTrace(&graph.classes[f->unsafeClass - 1u].usageTrace[LOCKDEP_USAGE_IRQS_ON]);
            break;
        case LOCKDEP_REPORT_NOT_HELD:
            outLine("  a lock is released that this CPU does not hold\n");
            break;
        case LOCKDEP_REPORT_HELD_OVERFLOW:
            outLine("  more than %u locks held at once\n", (unsigned)LOCKDEP_MAX_HELD);
            break;
        case LOCKDEP_REPORT_BAD_INIT:
            outLine(
                "  SPINLOCK_INIT on a non-static lock, or an uninitialized lock; use spinInit()\n");
            break;
        default:
            break;
    }
    klogRaw("  current stack:\n");
    backtracePrint(0, archFramePointer());
    printHeld(hs);
    if (expected) {
        expect.count++;
        expect.totalExpected++;
        return;
    }
    panic("lockdep: %s (report above)", kindName(f->kind));
}

static void disable(const char *what, uint32_t limit) {
    if (!ATOMIC_XCHG(&lockdepOff, true, MEM_RELAXED)) {
        klogWrite(KLOG_ERROR, "lockdep", "%s table full (%u); validator disabled", what, limit);
    }
}

static bool inKernelData(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= (uintptr_t)kernelDataStart && a < (uintptr_t)kernelDataEnd;
}

/* graphLock held. */
static LockdepVerdict resolveClass(LockdepMap *m, uint16_t *id) {
    if (m->classId != 0) {
        *id = m->classId;
        return LOCKDEP_OK;
    }
    const void *key = m->key != NULL ? (const void *)m->key : (const void *)m;
    if ((m->key == NULL && !inKernelData(m)) || m->name == NULL) {
        return LOCKDEP_REPORT_BAD_INIT;
    }
    LockdepVerdict v = lockdepCoreClassOf(&graph, key, m->name, id);
    if (v == LOCKDEP_OK) {
        m->classId = *id;
    }
    return v;
}

void lockdepAcquire(LockdepMap *m, bool trylock, uint64_t ip) {
    if (ATOMIC_LOAD(&lockdepOff, MEM_RELAXED) || panicInProgress()) {
        return;
    }
    /* Sampled before our own archIrqSave: after it IF is always 0 (D-186). */
    bool irqsOn = archInterruptsEnabled();
    uint8_t ctx = irqDepth() != 0 ? 1u : 0u;
    uint64_t flags = archIrqSave();
    CpuSync *s = cpuSync();
    if (s->lockdepRecursion != 0) {
        archIrqRestore(flags); /* the validator's own klog output: not validated */
        return;
    }
    s->lockdepRecursion = 1;
    rawSpinLock(&graphLock);

    LockdepFinding f;
    uint16_t id = 0;
    LockdepVerdict v = resolveClass(m, &id);
    if (v == LOCKDEP_FULL) {
        disable("class", LOCKDEP_MAX_CLASSES);
    } else if (v != LOCKDEP_OK) {
        f = (LockdepFinding){.kind = v};
        report(&f, &s->held);
    } else {
        v = lockdepCoreCheckAcquire(&graph, &s->held, m, id, ctx, irqsOn, trylock, &f);
        if (v != LOCKDEP_OK) {
            report(&f, &s->held);
        }
        if (lockdepCoreCommitAcquire(&graph, &s->held, m, id, ctx, irqsOn, trylock, v == LOCKDEP_OK,
                                     ip, captureTrace) == LOCKDEP_FULL) {
            disable("edge", LOCKDEP_MAX_EDGES);
        }
    }

    rawSpinUnlock(&graphLock);
    s->lockdepRecursion = 0;
    archIrqRestore(flags);
}

void lockdepRelease(LockdepMap *m, uint64_t ip) {
    (void)ip;
    if (panicInProgress()) {
        return;
    }
    bool irqsOn = archInterruptsEnabled(); /* still inside the caller's irqsave section if any */
    uint64_t flags = archIrqSave();
    CpuSync *s = cpuSync();
    if (s->lockdepRecursion != 0) {
        archIrqRestore(flags);
        return;
    }
    if (ATOMIC_LOAD(&lockdepOff, MEM_RELAXED)) {
        /* Disabled (a table filled): no checks any more, but a lock taken while the validator was
         * still on is on this CPU's held stack; drop it, or the D-187 balance checks (irqDispatch,
         * archTrapCatch, the ktest runner) would see it held forever. Only this CPU's stack changes
         * (IRQs off): irqsOn = false and no capture, so the graph is not touched. */
        lockdepCoreCommitRelease(&graph, &s->held, m, false, NULL);
        archIrqRestore(flags);
        return;
    }
    s->lockdepRecursion = 1;
    rawSpinLock(&graphLock);

    LockdepFinding f;
    LockdepVerdict v = lockdepCoreCheckRelease(&graph, &s->held, m, irqsOn, &f);
    if (v != LOCKDEP_OK) {
        report(&f, &s->held);
    }
    lockdepCoreCommitRelease(&graph, &s->held, m, irqsOn, captureTrace);

    rawSpinUnlock(&graphLock);
    s->lockdepRecursion = 0;
    archIrqRestore(flags);
}

bool lockdepActive(void) {
    return !ATOMIC_LOAD(&lockdepOff, MEM_RELAXED) && !panicInProgress();
}

bool lockdepIsHeld(const LockdepMap *m) {
    uint64_t flags = archIrqSave();
    const LockdepHeldStack *hs = &cpuSync()->held;
    bool held = false;
    for (uint32_t i = 0; i < hs->depth; i++) {
        if (hs->held[i].instance == m) {
            held = true;
        }
    }
    archIrqRestore(flags);
    return held;
}

uint32_t lockdepHeldDepth(void) {
    return cpuSync()->held.depth;
}

void lockdepExpectBegin(LockdepVerdict kind) {
    if (ktestCurrentName() == NULL || irqDepth() != 0 || expect.armed ||
        (kind != LOCKDEP_REPORT_INVERSION && kind != LOCKDEP_REPORT_RECURSION &&
         kind != LOCKDEP_REPORT_IRQ_INCONSISTENT && kind != LOCKDEP_REPORT_NOT_HELD &&
         kind != LOCKDEP_REPORT_IRQ_SAFE_UNSAFE)) {
        panic("lockdepExpectBegin: not in a ktest, in a handler, already armed, or bad kind");
    }
    uint64_t flags = archIrqSave();
    expect.kind = kind;
    expect.cpu = smpThisCpu();
    expect.count = 0;
    expect.armed = true;
    archIrqRestore(flags);
}

uint32_t lockdepExpectEnd(void) {
    uint64_t flags = archIrqSave();
    uint32_t n = expect.count;
    expect.armed = false;
    expect.count = 0;
    archIrqRestore(flags);
    return n;
}

bool lockdepExpectArmed(void) {
    return expect.armed;
}

static bool testOff;

void lockdepTestOff(void) {
    if (ktestCurrentName() == NULL || irqDepth() != 0 || testOff ||
        ATOMIC_LOAD(&lockdepOff, MEM_RELAXED)) {
        panic("lockdepTestOff: not in a ktest, in a handler, or the validator is already off");
    }
    testOff = true;
    ATOMIC_STORE(&lockdepOff, true, MEM_RELAXED);
}

void lockdepTestOn(void) {
    if (ktestCurrentName() == NULL || irqDepth() != 0 || !testOff) {
        panic("lockdepTestOn: not in a ktest, in a handler, or not turned off by lockdepTestOff");
    }
    testOff = false;
    ATOMIC_STORE(&lockdepOff, false, MEM_RELAXED);
}

bool lockdepTestOffReset(void) {
    if (!testOff) {
        return false;
    }
    testOff = false;
    ATOMIC_STORE(&lockdepOff, false, MEM_RELAXED);
    return true;
}

static bool nameEq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* graphLock held. 0 if there is no such class. */
static uint16_t classByName(const char *name) {
    for (uint32_t i = 0; i < graph.classCount; i++) {
        if (nameEq(graph.classes[i].name, name)) {
            return (uint16_t)(i + 1u);
        }
    }
    return 0;
}

bool lockdepClassRegistered(const char *name) {
    uint64_t flags = archIrqSave();
    rawSpinLock(&graphLock);
    bool found = classByName(name) != 0;
    rawSpinUnlock(&graphLock);
    archIrqRestore(flags);
    return found;
}

bool lockdepDependsOn(const char *fromName, const char *toName) {
    uint64_t flags = archIrqSave();
    rawSpinLock(&graphLock);
    uint16_t from = classByName(fromName), to = classByName(toName);
    bool dep = from != 0 && to != 0 && lockdepCoreDependsOn(&graph, from, to);
    rawSpinUnlock(&graphLock);
    archIrqRestore(flags);
    return dep;
}

void lockdepGetStats(LockdepStats *out) {
    uint64_t flags = archIrqSave();
    rawSpinLock(&graphLock);
    out->enabled = !ATOMIC_LOAD(&lockdepOff, MEM_RELAXED);
    out->classes = graph.classCount;
    out->edges = graph.edgeCount;
    out->expectedReports = expect.totalExpected;
    rawSpinUnlock(&graphLock);
    archIrqRestore(flags);
}

#else
typedef int lockdepReleaseEmptyTu; /* an empty translation unit is ISO-invalid */
#endif
