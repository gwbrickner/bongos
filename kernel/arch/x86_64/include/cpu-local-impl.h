/* x86 per-CPU arch state: the GDT, the TSS and the stack ranges of one CPU (ARCHITECTURE §7.1,
 * D-072/D-190), and the %gs-relative CpuLocal accessor. */
#ifndef KERNEL_ARCH_X86_64_CPU_LOCAL_IMPL_H
#define KERNEL_ARCH_X86_64_CPU_LOCAL_IMPL_H

#include "gdt.h"

#include <stddef.h>
#include <stdint.h>

struct CpuLocal;

/* SDM Vol 3A "64-Bit TSS Format". Packed, since the CPU reads these exact byte offsets;
 * `-Waddress-of-packed-member` means every field is read/written by value, never by `&field`. */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7]; /* ist[0] is IST1, ... ist[6] is IST7; only IST1-3 are used (D-073) */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomapBase;
} X86Tss;
_Static_assert(sizeof(X86Tss) == 104, "X86Tss must be the SDM's 104-byte 64-bit TSS layout");
_Static_assert(offsetof(X86Tss, rsp0) == 0x04, "X86Tss.rsp0 offset");
_Static_assert(offsetof(X86Tss, ist) == 0x24, "X86Tss.ist offset");
_Static_assert(offsetof(X86Tss, iomapBase) == 0x66, "X86Tss.iomapBase offset");

typedef struct ArchCpuLocal {
    uint64_t gdt[GDT_ENTRY_COUNT];
    X86Tss tss;
    uint64_t stackBottom, stackTop;   /* this CPU's boot/idle kernel stack */
    uint64_t istBottom[3], istTop[3]; /* IST1 (#DF), IST2 (NMI), IST3 (#MC) stacks (D-073) */
    int64_t tscOffset;                /* subtracted from this CPU's raw TSC (D-198) */
} ArchCpuLocal;

/* `mov %gs:0`: the CpuLocal `self` pointer. volatile so a future migration can't be CSE'd across.
 * No locks, IRQ-safe. */
static inline struct CpuLocal *archCpuLocal(void) {
    struct CpuLocal *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}

#endif
