/* Linker-provided symbols from kernel/arch/x86_64/kernel.ld (ARCHITECTURE §5.1/D-061). Each is
 * the address of a zero-length array: taking `&kernelTextStart` or using the name as an array
 * decays to the link-time address the linker chose, never dereferenced as data. No trailing
 * underscore/leading-underscore names (C17 §7.1.3 reserves file-scope `_x` identifiers), and
 * camelCase throughout (ARCHITECTURE §4) even though these come from the linker script rather
 * than a subsystem, since a leading OS/subsystem prefix would say nothing useful here. */
#ifndef KERNEL_SECTIONS_H
#define KERNEL_SECTIONS_H

#include "bootinfo.h" /* BOOTINFO_KERNEL_WINDOW_BASE, via kernel.mk's include path */

#include <stdint.h>

extern const uint8_t kernelImageStart[];
extern const uint8_t kernelImageEnd[];
extern const uint8_t kernelTextStart[];
extern const uint8_t kernelTextEnd[];
extern const uint8_t kernelRodataStart[];
extern const uint8_t kernelRodataEnd[];
extern const uint8_t kernelDataStart[];
extern const uint8_t kernelDataEnd[];
extern const uint8_t kernelBootStackBottom[];
extern const uint8_t kernelBootStackTop[];
extern const uint8_t kernelIst1Bottom[];
extern const uint8_t kernelIst1Top[];
extern const uint8_t kernelIst2Bottom[];
extern const uint8_t kernelIst2Top[];
extern const uint8_t kernelIst3Bottom[];
extern const uint8_t kernelIst3Top[];
extern const uint8_t ksymsStart[];
extern const uint8_t ksymsEnd[];

/* The KASLR slide this boot (ARCHITECTURE §5.5/§6.6, M2.6): how far the loader moved the kernel
 * image from its link address, i.e. where kernelImageStart really is minus where the linker put
 * it. Computed straight from the linker symbol, deliberately not from BootInfo, so it is valid at
 * any point (including inside panic() and the trap reporter, before or after the BootInfo copy
 * exists) and cannot be corrupted by a bad BootInfo. Link-time addresses (the KSYM blob, kernel.elf
 * for gdb) are `runtime address - kernelSlide()`. BOOTINFO_KERNEL_WINDOW_BASE must equal
 * KERNEL_LINK_BASE in kernel.ld (kernel.ld ASSERTs .text starts there; bootInfoCheckHeader checks
 * it against the loader's own report). Zero when the loader did not slide the image. */
static inline uint64_t kernelSlide(void) {
    return (uint64_t)(uintptr_t)kernelImageStart - BOOTINFO_KERNEL_WINDOW_BASE;
}

#endif
