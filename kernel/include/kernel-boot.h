/* Accessors for the kernel's own copy of the loader's handoff data (kernel/core/main.c), used by
 * ktests that need to inspect the live BootInfo/cmdline the kernel actually booted with. */
#ifndef KERNEL_KERNEL_BOOT_H
#define KERNEL_KERNEL_BOOT_H

#include "bootinfo.h"

/* The live, loader-supplied BootInfo pointer kernelMain validated and received (not a kernel-
 * owned copy: it still points into the original LOADER_RECLAIM page, which stays valid to read
 * until M2.2 reclaims it). Only valid from kernelMain's post-validation point on (i.e. from
 * ktests, which run after it). No locks; read-only. */
const BootInfo *kernelBootInfo(void);

/* The kernel's own NUL-terminated copy of the command line. Same availability as
 * kernelBootInfo(). No locks; read-only. */
const char *kernelCmdline(void);

#endif
