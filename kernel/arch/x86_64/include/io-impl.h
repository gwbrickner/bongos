/* x86 port I/O (SDM Vol 1 §3.6): the real implementation behind kernel/include/arch/io.h.
 * ARCHITECTURE §4 keeps x86 specifics only under kernel/arch/x86_64/; kernel.mk's include path
 * (-Ikernel/arch/x86_64/include) is what makes the portable wrapper header find this file by a
 * name distinct from its own (see the wrapper's own comment for why they aren't both "io.h"). */
#ifndef KERNEL_ARCH_X86_64_IO_IMPL_H
#define KERNEL_ARCH_X86_64_IO_IMPL_H

#include <stdint.h>

/* No locks, boot-time/IRQ-safe (a single instruction, no shared state): safe from any context. */
static inline void ioOutByte(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t ioInByte(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#endif
