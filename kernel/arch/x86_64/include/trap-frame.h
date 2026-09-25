/* The exception/interrupt frame trap-entry.asm's trapCommon builds before calling trapDispatch()
 * (ARCHITECTURE §7.2, D-074). Layout is dictated entirely by trapCommon's push order (GPRs
 * rax/rbx/rcx/rdx/rsi/rdi/rbp/r8-r15, in that order, on top of the stub's [vector, errorCode] and
 * the CPU's own [RIP, CS, RFLAGS, RSP, SS] hardware frame) -- these offsets and that push order
 * must never change independently of each other. */
#ifndef KERNEL_ARCH_X86_64_TRAP_FRAME_H
#define KERNEL_ARCH_X86_64_TRAP_FRAME_H

#include <stddef.h>
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t errorCode;
    /* Below here: the CPU's own hardware-pushed frame (SDM Vol 3A "64-Bit Mode Stack Frame"). */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} TrapFrame;

_Static_assert(sizeof(TrapFrame) == 176, "TrapFrame must be exactly 176 bytes");
_Static_assert(offsetof(TrapFrame, r15) == 0, "TrapFrame.r15 offset");
_Static_assert(offsetof(TrapFrame, rax) == 112, "TrapFrame.rax offset");
_Static_assert(offsetof(TrapFrame, vector) == 120, "TrapFrame.vector offset");
_Static_assert(offsetof(TrapFrame, errorCode) == 128, "TrapFrame.errorCode offset");
_Static_assert(offsetof(TrapFrame, rip) == 136, "TrapFrame.rip offset");
_Static_assert(offsetof(TrapFrame, cs) == 144, "TrapFrame.cs offset");
_Static_assert(offsetof(TrapFrame, rflags) == 152, "TrapFrame.rflags offset");
_Static_assert(offsetof(TrapFrame, rsp) == 160, "TrapFrame.rsp offset");
_Static_assert(offsetof(TrapFrame, ss) == 168, "TrapFrame.ss offset");

#endif
