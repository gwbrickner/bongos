/* x86 IDT/exception-dispatch internals (ARCHITECTURE §7.2, D-074): the real implementation behind
 * kernel/include/arch/trap.h, mirroring the cpu.h/cpu-impl.h split (D-062). */
#ifndef KERNEL_ARCH_X86_64_TRAP_IMPL_H
#define KERNEL_ARCH_X86_64_TRAP_IMPL_H

#include <stdint.h>

/* Builds and loads the 256-entry IDT (D-074), then re-enables faults from here on being reported
 * instead of triple-faulting. Called once, by cpu-init.c's archCpuInitBsp() -- must run *after*
 * LTR (an IST gate taken before TR is valid reads IST from garbage and triple-faults), which is
 * why IDT setup isn't part of archCpuInitBsp's own GDT/TSS step. No locks, boot-time-only. */
void trapIdtInit(void);

/* How many times #BP (int3) has fired and resumed since boot -- ARCHITECTURE §23's `int3 resumes`
 * ktest checks this went up by exactly 1. No locks (single-threaded, IF=0 throughout M2.1). */
uint64_t archBreakpointHits(void);

#endif
