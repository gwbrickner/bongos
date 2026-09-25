/* Portable-facing declarations for a handful of x86 primitives (ARCHITECTURE §2: "include/arch/
 * is the arch interface"; §4: "x86 specifics only in kernel/arch/x86_64/", "assembly only in
 * arch/ and boot/"). The real inline-asm implementations are x86-specific and live under
 * kernel/arch/x86_64/ (kernel/arch/x86_64/include/cpu-impl.h); this header is what the rest of the
 * kernel includes (`#include <arch/cpu.h>`). Mirrors the kernel/include/arch/io.h split (D-062):
 * kernel.mk puts kernel/arch/x86_64/include on the include path, and this file pulls the arch
 * backend in by a name distinct from its own so the quoted #include doesn't just find itself. */
#ifndef KERNEL_INCLUDE_ARCH_CPU_H
#define KERNEL_INCLUDE_ARCH_CPU_H

#include "cpu-impl.h"

/* Builds and installs the real GDT/TSS/IDT (ARCHITECTURE §7.1, D-072), replacing the loader-era
 * temporary tables. Called exactly once from kernelMain, before anything else touches interrupt
 * state; a second call panics. No locks; IF must be 0. BSP-only until M3.5 gives every AP its own
 * copy. */
void archCpuTablesInit(void);

#endif
