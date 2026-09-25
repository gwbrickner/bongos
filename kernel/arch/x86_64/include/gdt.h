/* x86_64 GDT selectors and the shared pseudo-descriptor layout (ARCHITECTURE §7.1, D-072). BSP-
 * only through M3.4 (SMP bring-up is M3.5): one static table, not one per CPU yet. */
#ifndef KERNEL_ARCH_X86_64_GDT_H
#define KERNEL_ARCH_X86_64_GDT_H

#include <stdint.h>

#define GDT_SEL_NULL      0x00
#define GDT_SEL_KERNEL_CS 0x08 /* matches entry.asm's temporary boot GDT (D-062) */
#define GDT_SEL_KERNEL_DS 0x10
#define GDT_SEL_USER_CS32 0x18 /* STAR-layout placeholder; descriptor is NOT present (D-072) */
#define GDT_SEL_USER_DS   0x20
#define GDT_SEL_USER_CS64 0x28
#define GDT_SEL_TSS       0x30 /* occupies two 8-byte slots (a 64-bit TSS descriptor is 16B) */

/* RPL3 forms of the user selectors, reserved now for when STAR is programmed (M5): SYSRET loads
 * CS from STAR[63:48]+16 and SS from STAR[63:48]+8, which is why the user descriptors sit in
 * this exact order (code32, data, code64) even though code32 is never actually used. */
#define GDT_SEL_USER_CS64_RPL3 (GDT_SEL_USER_CS64 | 3)
#define GDT_SEL_USER_DS_RPL3   (GDT_SEL_USER_DS | 3)

/* null(1) + kernel CS/DS(2) + user CS32/DS/CS64(3) + TSS(2 slots, a 64-bit TSS descriptor is
 * 16 bytes) = 8 qword slots: indices 0-5 are single-slot descriptors at offsets 0x00-0x28,
 * indices 6-7 are the TSS descriptor's low/high qwords at 0x30/0x38. GDTR limit is therefore
 * 8*8-1 = 0x3F, not the selector value of the last descriptor. */
#define GDT_ENTRY_COUNT 8

/* The pseudo-descriptor `lgdt`/`lidt` read: a 16-bit limit (size-1) followed by a 64-bit linear
 * base. Packed and exactly 10 bytes -- the CPU reads this memory layout directly. */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} X86DescriptorPtr;
_Static_assert(sizeof(X86DescriptorPtr) == 10,
               "X86DescriptorPtr must be the CPU's 10-byte pseudo-descriptor layout");

/* kernel/arch/x86_64/load-gdt.asm: loads `gdtr` and reloads every segment register from it. CS
 * can only be reloaded via a far jump/call/return, which needs real assembly (see the .asm file's
 * own comment). No locks, boot-time-only, not IRQ-safe (changes privileged CPU state). */
void archLoadGdt(const X86DescriptorPtr *gdtr);

#endif
