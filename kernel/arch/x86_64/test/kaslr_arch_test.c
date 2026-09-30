/* x86_64 KASLR ktests (M2.6, ARCHITECTURE §5.5/§6.6): the slide the loader reports agrees with
 * where the image really runs, and the loader's relocations were applied. They need x86 inline
 * assembly (RIP-relative `lea`, imm32 `mov`), so they live under kernel/arch/ (CLAUDE.md
 * "Placement"). Meaningful at every slide, and stay correct (vacuously for the relocation proofs)
 * when the loader chose slide 0 or `kaslr = off` was configured. */
#include "kernel-boot.h"
#include "ktest.h"
#include "sections.h"

#include <stdint.h>

#define BOOTINFO_KASLR_ALIGN 0x200000ULL /* the slide granule: 2 MiB (ARCHITECTURE §6.6) */

/* A function and a variable to take the address of. The function is a real text symbol
 * (R_X86_64_32S / _64 against .text), the variable a real .data symbol (against .data). */
static __attribute__((noinline, used)) void kaslrTestMarkerFn(void) {
    __asm__ volatile("" ::: "memory");
}
static volatile uint64_t kaslrTestMarkerData = 0x4B41534C52ULL; /* non-zero: .data, not .bss */

/* The loader tells the kernel its slide in BootInfo, while kernelSlide() derives it from
 * kernelImageStart (an absolute reference the loader slid); both must agree, be a legal slot, and
 * describe the address the code is actually running at. That last part is checked through a
 * RIP-relative `lea` (PC32, which no relocation touches), because comparing kernelImageStart with
 * BASE + kernelSlide() would only compare a value with itself. Comparing against BootInfo also
 * catches a kernelSlide() stuck at 0. */
KTEST(kaslr_slide_consistent) {
    const BootInfo *bi = kernelBootInfo();
    uint64_t slide = kernelSlide();
    uint64_t window = BOOTINFO_KERNEL_WINDOW_END - BOOTINFO_KERNEL_WINDOW_BASE;
    uint64_t needed = (bi->kernelSize + BOOTINFO_KASLR_ALIGN - 1) & ~(BOOTINFO_KASLR_ALIGN - 1);

    KTEST_ASSERT_EQ(bi->kaslrSlide, slide);
    KTEST_ASSERT_EQ(slide % BOOTINFO_KASLR_ALIGN, 0);
    KTEST_ASSERT(needed <= window && slide <= window - needed);
    KTEST_ASSERT_EQ(bi->kernelVirtBase, BOOTINFO_KERNEL_WINDOW_BASE + slide);
    uint64_t runningStart;
    __asm__ volatile("leaq %c1(%%rip), %0" : "=r"(runningStart) : "i"(kernelImageStart));
    KTEST_ASSERT_EQ(runningStart, BOOTINFO_KERNEL_WINDOW_BASE + bi->kaslrSlide);

    uint64_t fnAddr = (uint64_t)(uintptr_t)&kaslrTestMarkerFn;
    KTEST_ASSERT(fnAddr >= (uint64_t)(uintptr_t)kernelTextStart &&
                 fnAddr < (uint64_t)(uintptr_t)kernelTextEnd);
    KTEST_ASSERT(fnAddr - slide >= BOOTINFO_KERNEL_WINDOW_BASE &&
                 fnAddr - slide < BOOTINFO_KERNEL_WINDOW_BASE + bi->kernelSize);
}

/* Self-pointers: the linker stores the link-time address (R_X86_64_64), so unless the loader added
 * the slide the stored value points at the old location. Each is read through a volatile pointer
 * so the compiler cannot fold the comparison from the initializer. .rodata, .data, and a rodata
 * pointer at a text symbol are three distinct R_X86_64_64 targets. */
static const void *const kaslrRoSelf = (const void *)&kaslrRoSelf;
static const uint64_t kaslrRoFnAddr = (uint64_t)(uintptr_t)&kaslrTestMarkerFn;
static const void *volatile kaslrDataSelf = (const void *)&kaslrDataSelf;

/* `movq $sym, %reg` is a sign-extended imm32 (R_X86_64_32S) in this -mcmodel=kernel image, and the
 * loader must add the slide to it; `lea sym(%rip), %reg` is PC-relative (nothing to fix). Both
 * name the same symbol, so they agree only if the imm32 was slid. noinline + asm volatile keeps the
 * compiler from replacing either with the other, or with a constant. */
static __attribute__((noinline)) uint64_t kaslrAddrViaImm32(const void *sym, int which) {
    (void)sym;
    uint64_t r;
    if (which == 0) {
        __asm__ volatile("movq %1, %0" : "=r"(r) : "i"(&kaslrTestMarkerFn));
    } else {
        __asm__ volatile("movq %1, %0" : "=r"(r) : "i"(&kaslrTestMarkerData));
    }
    return r;
}

static __attribute__((noinline)) uint64_t kaslrAddrViaLea(int which) {
    uint64_t r;
    if (which == 0) {
        __asm__ volatile("leaq %c1(%%rip), %0" : "=r"(r) : "i"(&kaslrTestMarkerFn));
    } else {
        __asm__ volatile("leaq %c1(%%rip), %0" : "=r"(r) : "i"(&kaslrTestMarkerData));
    }
    return r;
}

KTEST(kaslr_relocs_applied) {
    /* R_X86_64_64 in .rodata and .data. */
    const void *const *volatile roPtr = &kaslrRoSelf;
    KTEST_ASSERT(*roPtr == (const void *)&kaslrRoSelf);
    const void *volatile *volatile dataPtr = &kaslrDataSelf;
    KTEST_ASSERT(*dataPtr == (const void *)&kaslrDataSelf);
    const uint64_t *volatile fnPtr = &kaslrRoFnAddr;
    KTEST_ASSERT_EQ(*fnPtr, (uint64_t)(uintptr_t)&kaslrTestMarkerFn);

    /* R_X86_64_32S in code, against .text and .data. */
    for (int which = 0; which < 2; which++) {
        uint64_t viaImm = kaslrAddrViaImm32(NULL, which);
        uint64_t viaLea = kaslrAddrViaLea(which);
        KTEST_ASSERT_EQ(viaImm, viaLea);
        KTEST_ASSERT(viaImm >= (uint64_t)(uintptr_t)kernelImageStart &&
                     viaImm < (uint64_t)(uintptr_t)kernelImageEnd);
    }
}
