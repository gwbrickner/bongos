/* x86 IDT/exception-dispatch internals (ARCHITECTURE §7.2, D-074): the real implementation behind
 * kernel/include/arch/trap.h, mirroring the cpu.h/cpu-impl.h split (D-062). */
#ifndef KERNEL_ARCH_X86_64_TRAP_IMPL_H
#define KERNEL_ARCH_X86_64_TRAP_IMPL_H

#include "trap-frame.h"

#include <stdint.h>

/* Builds and loads the 256-entry IDT (D-074), then re-enables faults from here on being reported
 * instead of triple-faulting. Called once, by cpu-init.c's archCpuInitBsp() -- must run *after*
 * LTR (an IST gate taken before TR is valid reads IST from garbage and triple-faults), which is
 * why IDT setup isn't part of archCpuInitBsp's own GDT/TSS step. No locks, boot-time-only. */
void trapIdtInit(void);

/* trap-entry.asm's trapCommon calls this once it has built a TrapFrame on the stack. Never called
 * from anywhere else, so no header outside this arch-internal one declares it. No locks; called
 * with IF=0 (no IRQ source is wired up before M3.2, so this never runs reentrantly); may not
 * sleep. `f` points into the interrupted context's own stack (or an IST stack for #DF/NMI/#MC) --
 * never touched after this returns or redirects it, since trapCommon's `iretq` reads it exactly
 * once more on the way out. */
void trapDispatch(TrapFrame *f);

#endif
