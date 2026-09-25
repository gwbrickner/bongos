/* Exception dispatch (ARCHITECTURE §7.1/§24, D-072/D-076). Called only from isr.asm's
 * isrCommon, once per taken exception (vectors 0-31 in M2.1; vectors 32-255 aren't present yet).
 */
#include "trap-frame.h"

#include "format.h"
#include "klog.h"
#include "ktest.h"
#include "panic.h"
#include "sections.h"

#include <arch/cpu.h>
#include <arch/trap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Struct TrapFrame is defined in trap-frame.h; these give panic.c (which only sees the opaque
 * TrapFrame from arch/trap.h) the two fields it needs to seed a backtrace. */
uint64_t archTrapFramePc(const TrapFrame *f) {
    return f->rip;
}

uint64_t archTrapFrameFp(const TrapFrame *f) {
    return f->rbp;
}

#define EXCEPTION_NAME_COUNT 32

static const char *const exceptionNames[EXCEPTION_NAME_COUNT] = {
    "#DE divide error",
    "#DB debug",
    "NMI",
    "#BP breakpoint",
    "#OF overflow",
    "#BR bound range exceeded",
    "#UD invalid opcode",
    "#NM device not available",
    "#DF double fault",
    "reserved",
    "#TS invalid TSS",
    "#NP segment not present",
    "#SS stack fault",
    "#GP general protection",
    "#PF page fault",
    "reserved",
    "#MF x87 floating-point",
    "#AC alignment check",
    "#MC machine check",
    "#XM SIMD floating-point",
    "#VE virtualization exception",
    "#CP control protection",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "#HV hypervisor injection",
    "#VC VMM communication",
    "#SX security exception",
    "reserved",
};

static const char *exceptionName(uint64_t vector) {
    if (vector < EXCEPTION_NAME_COUNT) {
        return exceptionNames[vector];
    }
    return "unknown";
}

static volatile uint32_t bpCount = 0;
static volatile uint64_t bpLastRip = 0;
static volatile uint64_t dfLastFrame = 0;

uint32_t archTrapBpCount(void) {
    return bpCount;
}

uint64_t archTrapBpLastRip(void) {
    return bpLastRip;
}

uint64_t archTrapDfLastFrame(void) {
    return dfLastFrame;
}

#define TRAP_EXPECT_INSN_MAX 15

static bool trapExpectArmed = false;
static uint32_t trapExpectVector = 0;
static uint8_t trapExpectInsn[TRAP_EXPECT_INSN_MAX];
static uint8_t trapExpectLen = 0;
static bool trapExpectFired = false;
static ArchTrapRecord trapExpectRecord;

Status archTrapExpect(uint32_t vector, const uint8_t *insn, uint8_t len) {
    if (!ktestIsActive()) {
        return STATUS_ERR_INVALID;
    }
    if (len == 0 || len > TRAP_EXPECT_INSN_MAX) {
        return STATUS_ERR_INVALID;
    }
    for (uint8_t i = 0; i < len; i++) {
        trapExpectInsn[i] = insn[i];
    }
    trapExpectVector = vector;
    trapExpectLen = len;
    trapExpectArmed = true;
    trapExpectFired = false;
    return STATUS_OK;
}

bool archTrapExpectTake(ArchTrapRecord *out) {
    if (!trapExpectFired) {
        return false;
    }
    *out = trapExpectRecord;
    trapExpectFired = false;
    return true;
}

static bool insnMatches(uint64_t rip, const uint8_t *insn, uint8_t len) {
    if (rip < (uint64_t)(uintptr_t)kernelTextStart ||
        (uint64_t)len > (uint64_t)(uintptr_t)kernelTextEnd - rip) {
        return false;
    }
    const uint8_t *at = (const uint8_t *)(uintptr_t)rip;
    for (uint8_t i = 0; i < len; i++) {
        if (at[i] != insn[i]) {
            return false;
        }
    }
    return true;
}

/* Decodes and prints the #PF error-code bits (SDM Vol 3A §4.7), or the selector-error bits for
 * #TS/#NP/#SS/#GP (SDM Vol 3A §6.13). */
static void printDecodedError(uint64_t vector, uint64_t errorCode) {
    char line[128];
    if (vector == 14) {
        ksnprintf(line, sizeof(line), "  pf: %s %s %s%s%s%s%s\n",
                  (errorCode & 1) ? "present" : "not-present", (errorCode & 2) ? "write" : "read",
                  (errorCode & 4) ? "user " : "", (errorCode & 8) ? "reserved-bit " : "",
                  (errorCode & 16) ? "fetch " : "", (errorCode & 32) ? "pk " : "",
                  (errorCode & (1ULL << 15)) ? "sgx " : "");
        klogRaw(line);
    } else if (vector == 10 || vector == 11 || vector == 12 || vector == 13) {
        ksnprintf(line, sizeof(line), "  selector error: index=%u ti=%u idt=%u ext=%u\n",
                  (unsigned)((errorCode >> 3) & 0x1FFF), (unsigned)((errorCode >> 2) & 1),
                  (unsigned)((errorCode >> 1) & 1), (unsigned)(errorCode & 1));
        klogRaw(line);
    }
}

void archTrapFrameDump(const TrapFrame *f) {
    char line[160];
    ksnprintf(line, sizeof(line), "  vector %llu (%s) error 0x%llx\n", f->vector,
              exceptionName(f->vector), f->errorCode);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  rip 0x%016llx cs 0x%04llx rflags 0x%016llx\n", f->rip, f->cs,
              f->rflags);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  rsp 0x%016llx ss 0x%04llx\n", f->rsp, f->ss);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  rax 0x%016llx rbx 0x%016llx rcx 0x%016llx\n", f->rax, f->rbx,
              f->rcx);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  rdx 0x%016llx rsi 0x%016llx rdi 0x%016llx\n", f->rdx, f->rsi,
              f->rdi);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  rbp 0x%016llx r8  0x%016llx r9  0x%016llx\n", f->rbp, f->r8,
              f->r9);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  r10 0x%016llx r11 0x%016llx r12 0x%016llx\n", f->r10, f->r11,
              f->r12);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  r13 0x%016llx r14 0x%016llx r15 0x%016llx\n", f->r13, f->r14,
              f->r15);
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  cr0 0x%016llx cr2 0x%016llx\n", archReadCr0(), archReadCr2());
    klogRaw(line);
    ksnprintf(line, sizeof(line), "  cr3 0x%016llx cr4 0x%016llx\n", archReadCr3(), archReadCr4());
    klogRaw(line);
    printDecodedError(f->vector, f->errorCode);
}

/* See trap-frame.h/arch/trap.h. Called by isr.asm for every taken exception; never itself
 * "returns" in the C sense for a fatal exception (panicTrap doesn't return). */
void archTrapDispatch(TrapFrame *f) {
    uint64_t cr2 = archReadCr2(); /* read before anything else could fault and change it */

    if (trapExpectArmed && f->vector == trapExpectVector && (f->cs & 3) == 0 &&
        insnMatches(f->rip, trapExpectInsn, trapExpectLen)) {
        trapExpectRecord.vector = (uint32_t)f->vector;
        trapExpectRecord.errorCode = f->errorCode;
        trapExpectRecord.rip = f->rip;
        trapExpectRecord.cr2 = cr2;
        trapExpectRecord.cs = f->cs;
        trapExpectRecord.rsp = f->rsp;
        trapExpectFired = true;
        trapExpectArmed = false;
        f->rip += trapExpectLen;
        return;
    }

    if (f->vector == 3) { /* #BP: never fatal (ARCHITECTURE §7.1/D-072) */
        bpCount++;
        bpLastRip = f->rip;
        klogWrite(KLOG_WARN, "trap", "int3 at 0x%016llx", f->rip);
        return;
    }

    if (f->vector == 8) { /* #DF: fatal, but record where before panicking */
        dfLastFrame = (uint64_t)(uintptr_t)f;
    }

    panicTrap(f, "exception %s (vector %llu), error 0x%llx", exceptionName(f->vector), f->vector,
              f->errorCode);
}
