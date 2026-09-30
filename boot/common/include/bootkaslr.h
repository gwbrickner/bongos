/* KASLR slide selection (ARCHITECTURE §6.6, M2.6, D-121): a pure function from the loader's
 * 64-byte random seed and the kernel image size to a 2 MiB-aligned offset inside the 512 MiB
 * kernel window. Shared by the UEFI loader and BIOS stage2, host-tested. */
#ifndef BOOT_COMMON_BOOTKASLR_H
#define BOOT_COMMON_BOOTKASLR_H

#include <stdint.h>

#include "boot-status.h"

#define BOOT_KASLR_SEED_BYTES 64u
#define BOOT_KASLR_ALIGN      0x200000ULL   /* 2 MiB: the slide is a whole number of large pages */
#define BOOT_KASLR_WINDOW     0x20000000ULL /* 512 MiB, BOOTINFO_KERNEL_WINDOW_END - _BASE */

/* Chooses a slide for a kernel image of `span` bytes (ElfImage.span) and stores it in `*outSlide`.
 * The slide is uniform over the aligned slots that keep the whole image inside the window: with
 * need = alignUp(span, 2 MiB) there are ((512 MiB - need) >> 21) + 1 slots, and slide = slot << 21.
 * The seed is hashed (splitmix64 finalizer over all 8 little-endian qwords), so every seed byte
 * matters and the same seed always yields the same slide. Returns BOOT_ERR_ELF_RANGE if span is 0
 * or larger than the window, BOOT_ERR_ELF_HEADER for NULL arguments; `*outSlide` is written only
 * on BOOT_OK. Only the slide is chosen here: physical placement of the image is independent of it.
 * No locks, never sleeps, boot-time or host-test only; pure; no 64-bit division. */
BootStatus bootKaslrPickSlide(const uint8_t seed[64], uint64_t span, uint64_t *outSlide);

#endif
