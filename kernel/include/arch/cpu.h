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

#endif
