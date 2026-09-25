/* x86_64 trap frame layout and arch-private trap plumbing (ARCHITECTURE §7.1, D-072/D-076).
 * Private to kernel/arch/x86_64/: isr.asm builds this struct on the stack, trap.c dispatches on
 * it, and kernel/arch/x86_64/ktests.c pokes the expected-trap mechanism directly. Portable code
 * only ever sees `TrapFrame` as an opaque type through kernel/include/arch/trap.h. */
#ifndef KERNEL_ARCH_X86_64_TRAP_FRAME_H
#define KERNEL_ARCH_X86_64_TRAP_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uapi/status.h"

/* Exact stack layout isr.asm's isrCommon builds before `call archTrapDispatch` (see isr.asm's
 * header comment for the derivation): 15 GPRs in push order low-to-high, then the stub-pushed
 * vector and error code, then the CPU-pushed hardware frame. Never reordered without updating
 * isr.asm's `lea rdi, [rsp + 16]` and the two _Static_asserts below in lockstep. */
typedef struct TrapFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, errorCode, rip, cs, rflags, rsp, ss;
} TrapFrame;

_Static_assert(sizeof(TrapFrame) == 176, "isr.asm's stack layout assumes a 176-byte TrapFrame");
_Static_assert(offsetof(TrapFrame, rip) == 136,
               "isr.asm reads the interrupted RIP at a fixed rsp+17*8 (136) offset");

/* One-shot expected-trap mechanism for ktests (D-076): arms a match on `vector` at a CPL0 `rip`
 * inside kernel text whose next `len` bytes equal `insn`. The next matching trap advances `rip`
 * past the instruction and resumes instead of calling panicTrap. Refuses (returns
 * STATUS_ERR_INVALID) unless ktestIsActive(). Disarms itself whether or not it ever fires; a
 * ktest must call archTrapExpectTake() to retrieve (and clear) a fired record. `len` must be
 * 1-15 (the longest possible x86 instruction). No locks, ktest-only, single-threaded (BSP-only in
 * M2.1). */
Status archTrapExpect(uint32_t vector, const uint8_t *insn, uint8_t len);

typedef struct ArchTrapRecord {
    uint32_t vector;
    uint64_t errorCode;
    uint64_t rip;
    uint64_t cr2;
    uint64_t cs;
    uint64_t rsp;
} ArchTrapRecord;

/* Returns true and fills `*out` if an armed archTrapExpect() has fired since the last call
 * (clearing the pending record either way it returns); false otherwise. No locks, ktest-only. */
bool archTrapExpectTake(ArchTrapRecord *out);

/* #BP (int3) hit counter and the RIP of the most recent one -- #BP is never fatal (ARCHITECTURE
 * §7.1/D-072): archTrapDispatch logs it and resumes. No locks; read/written only on the BSP in
 * M2.1. */
uint32_t archTrapBpCount(void);
uint64_t archTrapBpLastRip(void);

/* The TrapFrame pointer of the most recent #DF (double fault), or 0 if none has happened yet.
 * Used by the trap_df_on_ist1 ktest to confirm the #DF was actually delivered on the IST1 stack.
 * No locks. */
uint64_t archTrapDfLastFrame(void);

#endif
