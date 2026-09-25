/* x86 CPU primitives: the real implementation behind kernel/include/arch/cpu.h. ARCHITECTURE §4
 * keeps x86 specifics only under kernel/arch/x86_64/; kernel.mk's include path
 * (-Ikernel/arch/x86_64/include) is what makes the portable wrapper header find this file by a
 * name distinct from its own (see the wrapper's own comment for why they aren't both "cpu.h"). */
#ifndef KERNEL_ARCH_X86_64_CPU_IMPL_H
#define KERNEL_ARCH_X86_64_CPU_IMPL_H

#include <stdint.h>

/* No locks, boot-time/IRQ-safe (a single instruction, no shared state): safe from any context.
 * The "memory" clobber is load-bearing, not decoration: without it, the compiler is free to
 * reorder ordinary loads/stores across `cli` (it has no other side effect it can see), which
 * would silently break any caller relying on "no IRQ can run past this point" as an ordering
 * fence -- e.g. panic()'s own `panicking` recursion guard, and every irq-save lock built on this
 * later. */
static inline void archDisableInterrupts(void) {
    __asm__ volatile("cli" ::: "memory");
}

/* Saves RFLAGS and disables interrupts, returning the saved flags for archIrqRestore() -- the
 * building block for IRQ-safe critical sections (D-081: the pmm's lock is IRQ-disable-only until
 * a real spinlock exists, M3.4). IF stays 0 until M3.2 wires up the first IRQ source, so this is a
 * no-op in practice today, but every pmm entry point is written against the contract now so
 * turning IF on later needs no rewrite. No locks; safe from any context; "memory" clobber for the
 * same reordering reason as archDisableInterrupts(). */
static inline uint64_t archIrqSave(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

/* Restores interrupts to the state archIrqSave() captured (re-enables only if IF was set then).
 * No locks; safe from any context. */
static inline void archIrqRestore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

/* No locks; never returns. */
static inline _Noreturn void archHaltForever(void) {
    for (;;) {
        __asm__ volatile("cli" ::: "memory");
        __asm__ volatile("hlt" ::: "memory");
    }
}

/* Spin-wait hint (SDM Vol 2B `PAUSE`): reduces power/bus contention in a busy-wait loop and
 * avoids a memory-order mis-speculation penalty on exit from the loop. No locks, boot-time/IRQ-
 * safe: a single instruction with no side effect other than the hint itself. */
static inline void archPause(void) {
    __asm__ volatile("pause");
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

/* Loads the Task Register (SDM Vol 3A `LTR`) with a GDT selector -- must name a present TSS
 * descriptor (D-072: GDT_SEL_TSS) or this takes #GP. Sets the descriptor's busy bit; loading it
 * twice without an intervening jump/call through it takes #GP too (cpu-init.c guards against a
 * repeat call). No locks, boot-time-only, not IRQ-safe (changes privileged CPU state). */
static inline void archLoadTss(uint16_t selector) {
    __asm__ volatile("ltr %0" : : "r"(selector) : "memory");
}

/* SDM Vol 3A `LIDT`: loads the IDTR from a 10-byte pseudo-descriptor (gdt.h's X86DescriptorPtr).
 * No locks, boot-time-only, not IRQ-safe. */
static inline void archLoadIdt(const void *idtr) {
    __asm__ volatile("lidt %0" : : "m"(*(const uint8_t(*)[10])idtr) : "memory");
}

/* CR2 holds the faulting linear address after a #PF (SDM Vol 3A §4.7) -- only meaningful for
 * vector 14, and only until the *next* fault, so trapDispatch() reads this before anything else
 * that could itself fault. No locks, IRQ-safe (a single read, no shared state), pure with respect
 * to visible kernel state (CR2 itself is set by the CPU's fault delivery, not by this read). */
static inline uint64_t archReadCr2(void) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

/* CR0/CR3/CR4 for panic/trap register dumps (ARCHITECTURE §24). No locks, IRQ-safe, pure. */
static inline uint64_t archReadCr0(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}
static inline uint64_t archReadCr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
static inline uint64_t archReadCr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

/* SDM Vol 2 `RDTSC`: used only as an entropy stir-in for the one-time stack-canary derivation
 * (D-077) before the real CSPRNG exists -- not a timekeeping API (ARCHITECTURE §7.5 owns that,
 * M2.6+). No locks, IRQ-safe, boot-time. */
static inline uint64_t archReadTsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* SDM Vol 2 `CPUID`: `regs[0..3]` receive EAX/EBX/ECX/EDX for leaf `leaf`, subleaf `subleaf`
 * (ECX on entry; pass 0 for leaves that don't use it). No red-zone/PIC concerns here (the kernel
 * is built -fno-pic, -mno-red-zone), so EBX is an ordinary clobberable GPR. No locks, IRQ-safe,
 * pure with respect to visible kernel state. */
static inline void archCpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) {
    __asm__ volatile("cpuid"
                     : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                     : "a"(leaf), "c"(subleaf));
}

/* SDM Vol 2 `RDMSR`/`WRMSR`. No locks; not IRQ-safe (changes/reads privileged CPU state); caller's
 * responsibility that `msr` is valid on this CPU (an invalid MSR takes #GP, unhandled here). */
static inline uint64_t archRdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void archWrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

/* CR0/CR3/CR4 writes (ARCHITECTURE §6.3's PAT/CR4-protections/CR3-switch sequence, M2.3). The
 * "memory" clobber is load-bearing: these change how every subsequent memory access is
 * interpreted (paging mode, write-protect, cache-disable, ...), so the compiler must never
 * reorder an ordinary load/store across one. No locks; not IRQ-safe; boot-time/arch-init only. */
static inline void archWriteCr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" ::"r"(v) : "memory");
}
static inline void archWriteCr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" ::"r"(v) : "memory");
}
static inline void archWriteCr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" ::"r"(v) : "memory");
}

/* SDM Vol 2 `WBINVD`: writes back and invalidates every internal cache -- part of the PAT-
 * reprogramming procedure (SDM Vol 3A §11.11.8/§11.12.4, D-087). No locks; not IRQ-safe; slow
 * (flushes real hardware caches; under QEMU/TCG it's cheap). */
static inline void archWbinvd(void) {
    __asm__ volatile("wbinvd" ::: "memory");
}

/* SDM Vol 2 `INVLPG`: invalidates every TLB/paging-structure-cache entry for the page containing
 * `va` (global entries included). No locks; IRQ-safe; caller's responsibility that `va` is
 * canonical. */
static inline void archInvlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}

#endif
