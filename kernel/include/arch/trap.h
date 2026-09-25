/* Portable-facing trap/exception interface (ARCHITECTURE §2/§7.1, mirrors arch/cpu.h's split,
 * D-062/D-072). `TrapFrame` is opaque here -- its real layout is x86-specific
 * (kernel/arch/x86_64/include/trap-frame.h) and only the .c files under kernel/arch/x86_64/ ever
 * look inside it. Portable code (panic.c, backtrace.c) only needs a pointer plus these
 * accessors. */
#ifndef KERNEL_INCLUDE_ARCH_TRAP_H
#define KERNEL_INCLUDE_ARCH_TRAP_H

#include <stdint.h>

typedef struct TrapFrame TrapFrame;

/* The interrupted RIP and RBP out of a trap frame -- panicTrap (panic.c) uses these as the
 * starting point for a symbolized backtrace. No locks, called only while handling a trap
 * (interrupts already disabled). */
uint64_t archTrapFramePc(const TrapFrame *f);
uint64_t archTrapFrameFp(const TrapFrame *f);

/* Prints the trap frame's vector, decoded mnemonic/name, error code (decoded for #PF's page-fault
 * bits and #TS/#NP/#SS/#GP's selector-error bits), all GPRs, and CR0/CR2/CR3/CR4 to serial. No
 * locks; called only from panicTrap's context (interrupts already disabled). */
void archTrapFrameDump(const TrapFrame *f);

#endif
