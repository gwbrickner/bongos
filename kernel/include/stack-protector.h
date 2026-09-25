/* Stack-protector runtime (ARCHITECTURE §21/§6.6, D-077). `__stack_chk_guard`/`__stack_chk_fail`
 * are referenced by the compiler itself (no header needed for those); this declares the one
 * function kernelMain calls explicitly. */
#ifndef KERNEL_STACK_PROTECTOR_H
#define KERNEL_STACK_PROTECTOR_H

#include "bootinfo.h"

#include <stdint.h>

/* Seeds __stack_chk_guard from BootInfo.randomSeed (see stack-protector.c for the full
 * derivation/ordering contract). Must be called exactly once, from kernelMain, before any other
 * protected stack frame outlives it. No locks, boot-time only; panics if called twice. */
void stackGuardInit(const BootInfo *bi);

/* The compiler references this by this exact name (no declaration needed for its own codegen);
 * declared here only so ktests can read the live value (e.g. `stack_guard_seeded`, confirming
 * stackGuardInit() actually replaced the M1.3-era fixed constant). */
extern uintptr_t __stack_chk_guard;

#endif
