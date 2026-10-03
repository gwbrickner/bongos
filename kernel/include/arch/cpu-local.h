/* Portable-facing wrapper for the per-CPU arch state (ArchCpuLocal) and the GS-based accessor
 * archCpuLocal() (ARCHITECTURE §7.1, D-190); the x86 implementation is
 * kernel/arch/x86_64/include/cpu-local-impl.h. Same split as arch/cpu.h. */
#ifndef KERNEL_INCLUDE_ARCH_CPU_LOCAL_H
#define KERNEL_INCLUDE_ARCH_CPU_LOCAL_H

#include "cpu-local-impl.h"

#endif
