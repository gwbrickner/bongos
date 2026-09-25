/* Portable-facing declarations for exception dispatch (ARCHITECTURE §7.2). The implementation is
 * entirely x86-specific and lives in kernel/arch/x86_64/trap.c.
 */
#ifndef KERNEL_INCLUDE_ARCH_TRAP_H
#define KERNEL_INCLUDE_ARCH_TRAP_H

#include <stdint.h>

/* How many times #BP (int3) has fired and resumed since boot. No locks. */
uint64_t archBreakpointHits(void);

#endif
