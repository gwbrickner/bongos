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
#include "panic.h"

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

/* Prints the full fault report (vector, error code, CR2 if relevant, registers, control
 * registers, and a symbolized-once-KSYM-exists backtrace) then panics. Never returns. Deliberately
 * doesn't call panic() itself -- panic() prints its own unrelated backtrace and would duplicate
 * this one -- but shares panic()'s recursion guard and ktest-exit/halt tail (panicEnter()/
 * panicFinish()) so a fault-during-a-fault and a fault-during-a-plain-panic both get the same
 * "PANIC while already panicking" handling instead of two independent (and racing) code paths. */
static _Noreturn void trapReportAndPanic(const TrapFrame *f, uint64_t cr2) {
    if (!panicEnter()) {
        panicNested();
    }

    char errBuf[96];
    formatErrorCode((uint32_t)f->vector, f->errorCode, errBuf, sizeof(errBuf));

    char line[200];
    ksnprintf(line, sizeof(line), "exception %s (vector %llu), error=%s",
              vectorName((uint32_t)f->vector), (unsigned long long)f->vector, errBuf);
    char banner[256];
    ksnprintf(banner, sizeof(banner), "\r\nPANIC: %s\n", line);
    klogRaw(banner);

    if (f->vector == 14) {
        char cr2Line[48];
        ksnprintf(cr2Line, sizeof(cr2Line), "  CR2=0x%016llx\n", (unsigned long long)cr2);
        klogRaw(cr2Line);
    }

    char regLine[200];
    ksnprintf(regLine, sizeof(regLine),
              "  RIP=0x%016llx CS=0x%llx SS=0x%llx RFLAGS=0x%llx RSP=0x%016llx\n",
              (unsigned long long)f->rip, (unsigned long long)f->cs, (unsigned long long)f->ss,
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

    panicFinish(line);
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
