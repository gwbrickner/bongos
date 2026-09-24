/* x86 CPU primitives: the real implementation behind kernel/include/arch/cpu.h. ARCHITECTURE §4
 * keeps x86 specifics only under kernel/arch/x86_64/; kernel.mk's include path
 * (-Ikernel/arch/x86_64/include) is what makes the portable wrapper header find this file by a
 * name distinct from its own (see the wrapper's own comment for why they aren't both "cpu.h"). */
#ifndef KERNEL_ARCH_X86_64_CPU_IMPL_H
#define KERNEL_ARCH_X86_64_CPU_IMPL_H

#include <stdint.h>

/* No locks, boot-time/IRQ-safe (a single instruction, no shared state): safe from any context. */
static inline void archDisableInterrupts(void) {
    __asm__ volatile("cli");
}

/* No locks; never returns. */
static inline _Noreturn void archHaltForever(void) {
    for (;;) {
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}

/* Returns the caller's own `rbp`, for a raw frame-pointer backtrace walk (panic.c). Marked
 * always_inline (not just the usual `static inline` hint): if this ever compiled as a real
 * out-of-line call, the `mov %rbp` inside it would read *this function's own* frame, one level
 * deeper than the caller's -- silently shifting every backtrace by one frame. Only meaningful
 * when built with -fno-omit-frame-pointer, which the kernel always is. No locks; IRQ-safe; pure.
 */
static inline __attribute__((always_inline)) uint64_t archFramePointer(void) {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    return rbp;
}

#endif
