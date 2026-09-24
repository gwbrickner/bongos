/* Portable-facing declaration for the QEMU isa-debug-exit protocol (ARCHITECTURE §23). The
 * implementation is x86-specific (a single `outb 0xF4`) and lives in kernel/arch/x86_64/qemu.c. */
#ifndef KERNEL_INCLUDE_ARCH_QEMU_H
#define KERNEL_INCLUDE_ARCH_QEMU_H

#include <stdint.h>

/* Writes `code` to the isa-debug-exit port (0xF4) and halts; QEMU maps this to process exit code
 * 2*code+1 (0x10 -> 33, 0x11 -> 35, ARCHITECTURE §23). Callers write this port only when `ktest=`
 * is present on the command line -- it isn't a defined port on real hardware. No locks, boot-time
 * or ktest-only; never returns. */
_Noreturn void archDebugExit(uint8_t code);

#endif
