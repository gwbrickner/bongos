/* See kernel/include/arch/trap.h and trap-impl.h. IDT construction + the C-level exception
 * handler (ARCHITECTURE §7.2, D-074). Every unhandled vector panics in M2.1 except #BP (a trap,
 * not a fault -- the saved RIP already points past the `int3` byte, so resuming is a plain
 * `iretq` with the frame unchanged, never an RIP adjustment). NMI/#DF/#MC always panic: no
 * legitimate source exists yet, and CR4.MCE isn't set until M3.6. IF stays 0 for all of M2.1 (no
 * IRQ source is wired up before M3.2), so nothing here needs to be reentrant. */
#include "include/trap-impl.h"

#include "include/cpu-impl.h"
#include "include/gdt.h"
#include "include/trap-frame.h"

#include "backtrace.h"
#include "format.h"
#include "klog.h"
#include "ksym.h"
#include "ktest.h"
#include "panic.h"
#include "sections.h"

#include <arch/trap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One pointer per vector, filled in by trap-entry.asm's %rep-generated stub table. */
extern const uint64_t trapStubTable[256];

#define IDT_VECTOR_COUNT        256
#define IDT_ATTR_INTERRUPT_DPL0 0x8E /* P=1, DPL=0, type=0xE (32/64-bit interrupt gate) */
#define IDT_ATTR_INTERRUPT_DPL3 0xEE /* same, DPL=3 -- vector 3 only, for a future user int3 */

/* SDM Vol 3A Table 6-1: short mnemonic + name for vectors 0-31. Vectors 32+ have no architectural
 * name (device IRQs, ARCHITECTURE §7.2's vector map). */
static const char *vectorName(uint32_t vector) {
    static const char *const names[32] = {
        "#DE Divide Error",
        "#DB Debug",
        "NMI Non-Maskable Interrupt",
        "#BP Breakpoint",
        "#OF Overflow",
        "#BR BOUND Range Exceeded",
        "#UD Invalid Opcode",
        "#NM Device Not Available",
        "#DF Double Fault",
        "Coprocessor Segment Overrun",
        "#TS Invalid TSS",
        "#NP Segment Not Present",
        "#SS Stack-Segment Fault",
        "#GP General Protection",
        "#PF Page Fault",
        "reserved",
        "#MF x87 FPU Error",
        "#AC Alignment Check",
        "#MC Machine Check",
        "#XM SIMD Floating-Point Exception",
        "#VE Virtualization Exception",
        "#CP Control Protection Exception",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "#HV Hypervisor Injection",
        "#VC VMM Communication",
        "#SX Security Exception",
        "reserved",
    };
    return vector < 32 ? names[vector] : "unregistered vector";
}

static uint64_t breakpointHits = 0;

uint64_t archBreakpointHits(void) {
    return breakpointHits;
}

/* Appends the #PF error-code bit breakdown (SDM Vol 3A §4.7) or a selector-error breakdown (SDM
 * Vol 3A §6.13, vectors 10/11/12/13) to `buf`. Every other vector's error code (if any) is just
 * printed as a raw hex value by the caller. */
static void formatErrorCode(uint32_t vector, uint64_t errorCode, char *buf, size_t bufSize) {
    if (vector == 14) {
        ksnprintf(buf, bufSize, "0x%llx [P=%d W=%d U=%d RSVD=%d I=%d PK=%d SS=%d]",
                  (unsigned long long)errorCode, (int)(errorCode & 1), (int)((errorCode >> 1) & 1),
                  (int)((errorCode >> 2) & 1), (int)((errorCode >> 3) & 1),
                  (int)((errorCode >> 4) & 1), (int)((errorCode >> 5) & 1),
                  (int)((errorCode >> 6) & 1));
    } else if (vector == 10 || vector == 11 || vector == 12 || vector == 13) {
        ksnprintf(buf, bufSize, "0x%llx [EXT=%d IDT=%d TI=%d INDEX=0x%llx]",
                  (unsigned long long)errorCode, (int)(errorCode & 1), (int)((errorCode >> 1) & 1),
                  (int)((errorCode >> 2) & 1), (unsigned long long)((errorCode >> 3) & 0x1FFF));
    } else {
        ksnprintf(buf, bufSize, "0x%llx", (unsigned long long)errorCode);
    }
}

/* archTrapCatch's fault-catching mechanism (D-078): a setjmp/longjmp-style register-context
 * save/resume so a ktest can deliberately trigger a real #PF/#UD/etc. and prove the kernel
 * detects it, without ending the whole ktest run. archTrapCatchCall/archTrapCatchResume
 * (trap-entry.asm) save/restore the callee-saved registers + RSP, *and* the return address
 * archTrapCatchCall was itself called with (`retAddr`) -- archTrapCatchResume jumps to that saved
 * value directly rather than `ret`-ing through whatever's on the stack at `[ctx.rsp]`, so a wild
 * write in `fn` that reaches that far up the stack (exactly the bug the first version of
 * stack_protector_detects_smash hit, turning a clean catch into a real #GP -- see docs/logs/
 * M2.1.md) can't corrupt the resume target. TrapCatchCtx's field order and offsets must stay in
 * sync with trap-entry.asm's hand-written offsets -- enforced below, not just commented, via
 * _Static_assert on every field's offsetof(). */
typedef struct {
    uint64_t rsp, rbx, rbp, r12, r13, r14, r15, retAddr;
} TrapCatchCtx;
_Static_assert(sizeof(TrapCatchCtx) == 64, "TrapCatchCtx must be 8 qwords (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, rsp) == 0, "TrapCatchCtx.rsp offset (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, rbx) == 8, "TrapCatchCtx.rbx offset (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, rbp) == 16, "TrapCatchCtx.rbp offset (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, r12) == 24, "TrapCatchCtx.r12 offset (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, r13) == 32, "TrapCatchCtx.r13 offset (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, r14) == 40, "TrapCatchCtx.r14 offset (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, r15) == 48, "TrapCatchCtx.r15 offset (trap-entry.asm)");
_Static_assert(offsetof(TrapCatchCtx, retAddr) == 56,
               "TrapCatchCtx.retAddr offset (trap-entry.asm)");

extern uint32_t archTrapCatchCall(TrapCatchCtx *ctx, void (*fn)(void *), void *arg);
extern _Noreturn void archTrapCatchResume(TrapCatchCtx *ctx);

static struct {
    TrapCatchCtx ctx;
    uint64_t mask;
    bool armed;
    TrapCatchInfo info;
} trapCatch;

/* Vectors D-074 never lets archTrapCatch claim: NMI/#DF/#MC stay always-fatal (no legitimate
 * source exists yet, and CR4.MCE isn't set until M3.6). #BP is excluded separately below (not a
 * "forbidden" vector in the same sense -- trapDispatch() just resumes it before archTrapCatch ever
 * gets a look, so a catch for it could never fire). */
#define TRAP_CATCH_FORBIDDEN_MASK (TRAP_CATCH_VEC(2) | TRAP_CATCH_VEC(8) | TRAP_CATCH_VEC(18))
/* Every bit archTrapCatch actually knows how to honor: vectors 0-31, plus the three software
 * kinds. */
#define TRAP_CATCH_VALID_MASK                                                                      \
    (0xFFFFFFFFULL | TRAP_CATCH_STACK_SMASH | TRAP_CATCH_UBSAN | TRAP_CATCH_KERNEL_BUG)

/* Disarms any in-progress catch without resuming anywhere -- used by the panic path (trap.c below)
 * so a fatal fault that isn't itself a match can't leave a stale armed catch around to wrongly
 * claim an unrelated *later* fault (e.g. one that happens while printing this fault's own report).
 * No-op if nothing is armed. */
static void trapCatchDisarm(void) {
    trapCatch.armed = false;
}

bool archTrapCatch(uint64_t mask, void (*fn)(void *), void *arg, TrapCatchInfo *out) {
    if (!ktestIsActive()) {
        panic("archTrapCatch: called outside a ktest run");
    }
    if (trapCatch.armed) {
        panic("archTrapCatch: already armed (no nesting)");
    }
    if ((mask & TRAP_CATCH_FORBIDDEN_MASK) != 0) {
        panic("archTrapCatch: cannot catch NMI/#DF/#MC");
    }
    if ((mask & TRAP_CATCH_VEC(3)) != 0) {
        panic("archTrapCatch: cannot catch #BP (trapDispatch resumes it unconditionally)");
    }
    if ((mask & ~TRAP_CATCH_VALID_MASK) != 0) {
        panic("archTrapCatch: mask has unrecognized bits set");
    }

    trapCatch.mask = mask;
    trapCatch.armed = true;
    uint32_t caught = archTrapCatchCall(&trapCatch.ctx, fn, arg);
    trapCatch.armed = false; /* one-shot; also covers the "returned without faulting" case */
    if (caught != 0 && out != NULL) {
        *out = trapCatch.info;
    }
    return caught != 0;
}

/* no_sanitize("undefined"): called from ubsan.c's report() *before* its own recursion guard is
 * set (so a caught trip never trips that guard for a later, unrelated real trip -- D-078/docs/
 * logs/M2.1.md). If this function's own code somehow tripped UBSan, it would recurse into
 * report() with the guard still clear, so it's excluded from instrumentation entirely rather than
 * relying on never having UB in the first place. */
__attribute__((no_sanitize("undefined"))) bool archTrapCatchSoftware(uint64_t kind, uint64_t pc) {
    if (!trapCatch.armed || (trapCatch.mask & kind) == 0) {
        return false;
    }
    trapCatch.armed = false;
    trapCatch.info =
        (TrapCatchInfo){.kind = kind, .vector = 0, .errorCode = 0, .cr2 = 0, .rip = pc};
    archTrapCatchResume(&trapCatch.ctx);
}

/* Prints the fault report (vector, error code, CR2 if relevant, registers, control registers, and
 * a symbolized backtrace) under `header` ("PANIC" or, for a ktest-armed catch, "TRAP (expected,
 * caught by ktest)" -- D-078 wants this backtrace exercised on *every* fault, caught or not, not
 * just real panics). Writes the built "exception ... error=..." description into `lineOut` (used
 * by the panic path for panicFinish()'s ktest-fail message; the catch path has no use for it but
 * takes the same buffer for symmetry). Doesn't itself decide panic vs. resume -- see
 * trapReportAndPanic() and archTrapCatchTryResume() below. */
static void trapPrintReport(const char *header, const TrapFrame *f, uint64_t cr2, char *lineOut,
                            size_t lineOutCap) {
    char errBuf[96];
    formatErrorCode((uint32_t)f->vector, f->errorCode, errBuf, sizeof(errBuf));
    ksnprintf(lineOut, lineOutCap, "exception %s (vector %llu), error=%s",
              vectorName((uint32_t)f->vector), (unsigned long long)f->vector, errBuf);
    char banner[256];
    ksnprintf(banner, sizeof(banner), "\r\n%s: %s\n", header, lineOut);
    klogRaw(banner);

    if (f->vector == 14) {
        char cr2Line[48];
        ksnprintf(cr2Line, sizeof(cr2Line), "  CR2=0x%016llx\n", (unsigned long long)cr2);
        klogRaw(cr2Line);
    }

    char sym[80];
    ksymSymbolize(f->rip, sym, sizeof(sym));
    char regLine[200];
    ksnprintf(regLine, sizeof(regLine),
              "  RIP=0x%016llx <%s> CS=0x%llx SS=0x%llx RFLAGS=0x%llx RSP=0x%016llx\n",
              (unsigned long long)f->rip, sym, (unsigned long long)f->cs, (unsigned long long)f->ss,
              (unsigned long long)f->rflags, (unsigned long long)f->rsp);
    klogRaw(regLine);
    ksnprintf(regLine, sizeof(regLine),
              "  RAX=0x%016llx RBX=0x%016llx RCX=0x%016llx RDX=0x%016llx\n",
              (unsigned long long)f->rax, (unsigned long long)f->rbx, (unsigned long long)f->rcx,
              (unsigned long long)f->rdx);
    klogRaw(regLine);
    ksnprintf(regLine, sizeof(regLine),
              "  RSI=0x%016llx RDI=0x%016llx RBP=0x%016llx R8= 0x%016llx\n",
              (unsigned long long)f->rsi, (unsigned long long)f->rdi, (unsigned long long)f->rbp,
              (unsigned long long)f->r8);
    klogRaw(regLine);
    ksnprintf(regLine, sizeof(regLine),
              "  R9= 0x%016llx R10=0x%016llx R11=0x%016llx R12=0x%016llx\n",
              (unsigned long long)f->r9, (unsigned long long)f->r10, (unsigned long long)f->r11,
              (unsigned long long)f->r12);
    klogRaw(regLine);
    ksnprintf(regLine, sizeof(regLine), "  R13=0x%016llx R14=0x%016llx R15=0x%016llx\n",
              (unsigned long long)f->r13, (unsigned long long)f->r14, (unsigned long long)f->r15);
    klogRaw(regLine);
    ksnprintf(regLine, sizeof(regLine), "  CR0=0x%016llx CR3=0x%016llx CR4=0x%016llx\n",
              (unsigned long long)archReadCr0(), (unsigned long long)archReadCr3(),
              (unsigned long long)archReadCr4());
    klogRaw(regLine);

    klogRaw("Backtrace:\n");
    backtracePrint(f->rip, f->rbp);
}

/* Prints the full fault report then panics. Never returns. Deliberately doesn't call panic()
 * itself -- panic() prints its own unrelated backtrace and would duplicate this one -- but shares
 * panic()'s recursion guard and ktest-exit/halt tail (panicEnter()/panicFinish()) so a
 * fault-during-a-fault and a fault-during-a-plain-panic both get the same "PANIC while already
 * panicking" handling instead of two independent (and racing) code paths.
 *
 * Disarms any still-armed archTrapCatch() first, even though this path means the *current* fault
 * wasn't a match (archTrapCatchTryResume() already checked): otherwise a catch armed for one
 * vector left armed across an unrelated fatal fault (say, a #DF while a #PF catch is still armed)
 * would wrongly claim a *later* fault of the matching vector that happens while this very report
 * is being printed, misreporting cascading damage as a clean catch instead of the fatal panic it
 * actually is. */
static _Noreturn void trapReportAndPanic(const TrapFrame *f, uint64_t cr2) {
    trapCatchDisarm();

    if (!panicEnter()) {
        panicNested();
    }
    char line[200];
    trapPrintReport("PANIC", f, cr2, line, sizeof(line));
    panicFinish(line);
}

/* Called from trapDispatch() before it would otherwise panic. Returns true (and has already
 * redirected `f` to resume via archTrapCatchResume once trapCommon's iretq runs) if a ktest-armed
 * catch claims this fault; false means trapDispatch should panic as usual. Only catches faults
 * that occurred in kernel mode (there's no ring 3 yet, but this is the same check M4/M5's user
 * fault paths will need, so it's here from the start), vectors 0-31 (TRAP_CATCH_VEC's range), and
 * with `f->rsp` inside the boot stack at or below where archTrapCatch's own call frame lives
 * (`trapCatch.ctx.rsp`) -- defense in depth against catching a fault that happened somewhere
 * archTrapCatchResume couldn't sanely unwind back from (M2.1 only ever runs `fn` on the boot
 * stack; IST1-3 are reserved for #DF/NMI/#MC, both forbidden vectors archTrapCatch can never arm
 * for, so a legitimate catch's `f->rsp` is always on the boot stack too). */
static bool archTrapCatchTryResume(TrapFrame *f, uint64_t cr2) {
    if (!trapCatch.armed || (f->cs & 3) != 0 || f->vector >= 32) {
        return false;
    }
    uint64_t bit = TRAP_CATCH_VEC((uint32_t)f->vector);
    if ((trapCatch.mask & bit) == 0) {
        return false;
    }
    if (f->rsp < (uint64_t)(uintptr_t)kernelBootStackBottom || f->rsp > trapCatch.ctx.rsp) {
        return false;
    }

    /* Disarm and snapshot *before* printing: a fault taken while trapPrintReport()/
     * backtracePrint() themselves run (a bug in the report path, or another legitimate fault)
     * must never be mistaken for a second catch of the original armed vector. */
    trapCatch.armed = false;
    trapCatch.info = (TrapCatchInfo){
        .kind = bit, .vector = f->vector, .errorCode = f->errorCode, .cr2 = cr2, .rip = f->rip};

    char line[200];
    trapPrintReport("TRAP (expected, caught by ktest)", f, cr2, line, sizeof(line));

    f->rip = (uint64_t)(uintptr_t)archTrapCatchResume;
    f->rsp = trapCatch.ctx.rsp;
    f->rdi = (uint64_t)(uintptr_t)&trapCatch.ctx;
    /* Clear TF (never resume mid-single-step into a caught fault), DF (SysV requires DF=0 on
     * return into archTrapCatch's C caller, which archTrapCatchResume's `jmp` effectively is), NT
     * (set, a 64-bit `iretq` -- trapCommon's own, on the way into this function -- would #GP), and
     * AC (irrelevant until SMAP, M2.3, but there's no reason to let a caught fault's stale AC leak
     * into whatever runs next). */
    f->rflags &= ~((1ULL << 8) | (1ULL << 10) | (1ULL << 14) | (1ULL << 18));
    return true;
}

void trapDispatch(TrapFrame *f) {
    /* First statement, before anything else that could itself fault (SDM Vol 3A §4.7: CR2 holds
     * the faulting address only until the *next* page fault). */
    uint64_t cr2 = archReadCr2();

    if (f->vector == 3) {
        breakpointHits++;
        klogWrite(KLOG_DEBUG, "trap", "int3 at 0x%016llx", (unsigned long long)f->rip);
        return; /* trapCommon's iretq resumes with the frame unchanged -- RIP already advanced */
    }

    if (archTrapCatchTryResume(f, cr2)) {
        return; /* trapCommon's iretq now lands in archTrapCatchResume instead */
    }

    trapReportAndPanic(f, cr2);
}

/* SDM Vol 3A "64-Bit Mode IDT Gate Descriptors": low qword packs offset[15:0], the segment
 * selector, the IST index, the type/attribute byte, and offset[31:16]; the high qword is
 * offset[63:32] (bits 96-127 of the full descriptor are reserved/0, left unset here). */
static void idtGateSet(uint64_t idt[IDT_VECTOR_COUNT * 2], uint32_t vector, uint64_t offset,
                       uint8_t ist, uint8_t attr) {
    uint64_t low = (offset & 0xFFFFULL) | ((uint64_t)GDT_SEL_KERNEL_CS << 16) |
                   ((uint64_t)ist << 32) | ((uint64_t)attr << 40) |
                   (((offset >> 16) & 0xFFFFULL) << 48);
    uint64_t high = offset >> 32;
    idt[vector * 2] = low;
    idt[vector * 2 + 1] = high;
}

/* 4096-aligned even though the CPU doesn't require it: M2.3 may remap the IDT read-only, and a
 * page-aligned start makes that a clean single-page (for now) mapping change rather than sharing
 * a page with unrelated .bss. */
static uint64_t idt[IDT_VECTOR_COUNT * 2] __attribute__((aligned(4096)));

void trapIdtInit(void) {
    for (uint32_t v = 0; v < IDT_VECTOR_COUNT; v++) {
        uint8_t ist = 0;
        if (v == 8) {
            ist = 1; /* #DF */
        } else if (v == 2) {
            ist = 2; /* NMI */
        } else if (v == 18) {
            ist = 3; /* #MC */
        }
        uint8_t attr = (v == 3) ? IDT_ATTR_INTERRUPT_DPL3 : IDT_ATTR_INTERRUPT_DPL0;
        idtGateSet(idt, v, trapStubTable[v], ist, attr);
    }

    X86DescriptorPtr idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)(uintptr_t)idt,
    };
    archLoadIdt(&idtr);
}
