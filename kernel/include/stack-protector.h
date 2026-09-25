/* Stack-protector guard seeding (ARCHITECTURE §6.6, D-074). */
#ifndef KERNEL_STACK_PROTECTOR_H
#define KERNEL_STACK_PROTECTOR_H

#include <stdbool.h>
#include <stdint.h>

/* Derives __stack_chk_guard from the loader's 64-byte boot seed (BootInfo.randomSeed) with a
 * one-way mix, and installs it. Call exactly once, directly from kernelMain (itself marked
 * no_stack_protector), right after bootInfoValidate() accepts the BootInfo, while the only
 * frames on the stack are kernelEntry (asm, no canary) and kernelMain -- every frame that could
 * be live across the reseed must tolerate __stack_chk_guard changing under it, and kernelMain is
 * the only one that does (it's marked no_stack_protector too). Never call this a second time,
 * not even once M2.6's CSPRNG exists (D-074): any frame still live at a second reseed would
 * immediately fail its own canary check. IF must be 0. Returns false (having still installed
 * *some* nonzero guard, derived from the TSC instead) if `seed` was all-zero or the fold
 * collapsed to zero -- the caller should log a WARN in that case. No locks; not reentrant;
 * cannot otherwise fail. */
__attribute__((no_stack_protector, noinline)) bool stackGuardInit(const uint8_t seed[64]);

#endif
