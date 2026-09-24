/* Small portable helpers shared by every boot/common/ translation unit (ARCHITECTURE §3: this
 * code builds for x86_64-windows now, i386-elf in M2.5, and the host test binary). D-065 records
 * the portability rules these exist for: never `long` or 64-bit `/`/`%` (i386 would pull in
 * `__udivdi3`; every divisor used here is a power of two so shifts/masks suffice), and pointers
 * are converted from a physical address only through bootPhysToPtr(). */
#ifndef BOOT_COMMON_BOOTMEM_H
#define BOOT_COMMON_BOOTMEM_H

#include <stddef.h>
#include <stdint.h>

/* Rounds `x` up/down to a multiple of `align`, which must be a power of two. No locks, boot-time
 * or host-test only; pure. */
static inline uint64_t bootAlignUp(uint64_t x, uint64_t align) {
    return (x + (align - 1)) & ~(align - 1);
}
static inline uint64_t bootAlignDown(uint64_t x, uint64_t align) {
    return x & ~(align - 1);
}

/* The only place a physical address becomes a pointer (D-065): before ExitBootServices, UEFI
 * firmware runs with every physical address identity-mapped, so this is a plain cast -- but
 * writing it this way means a later loader (BIOS stage2's protected-mode phase, or a host test
 * standing in a fake physical space) only has one call site to change. No locks, boot-time or
 * host-test only. */
static inline void *bootPhysToPtr(uint64_t phys) {
    return (void *)(uintptr_t)phys;
}

/* Freestanding-safe memcpy/memset: boot/common/ avoids libc names (D-065) since the UEFI target
 * has no libc and clang's implicit `memcpy`/`memset` calls are shimmed separately
 * (boot/uefi/libc-shim.c) for struct assignment/zero-init, not for explicit copies like this.
 * No locks, boot-time or host-test only. */
void bootMemcpy(void *dst, const void *src, size_t n);
void bootMemset(void *dst, uint8_t value, size_t n);

#endif
