/* The loaders' KASLR step (M2.6, ARCHITECTURE §6.6, D-120/D-121), shared by the UEFI loader and
 * BIOS stage2 so the slide/relocate/fall-back sequence and its serial log lines cannot drift
 * between them. Lives with the serial helpers (boot/common/hw/) because it logs. */
#ifndef LOADER_KASLR_H
#define LOADER_KASLR_H

#include <stdbool.h>
#include <stdint.h>

#include "boot-status.h"
#include "elf64.h"

/* Runs right after elfLoad(img, file, dest) succeeded. If `kaslrOn` is false it logs
 * `loader: kaslr: off (boot.cfg); base=0x<linkBase>` and never touches `dest`. Otherwise it picks
 * a slide from `seed` (the loader's 64 random bytes, bootKaslrPickSlide) and rebases `dest` with
 * elfRelocate; success logs `loader: kaslr: slide=0x%016llx base=0x%016llx relocs=%u` and stores
 * the slide in `*outSlide`. On ANY failure of the pick or the relocation it logs
 * `loader: kaslr: disabled: <status>; base=0x<linkBase>`, discards `dest` by re-running
 * elfLoad(img, file, dest) (elfRelocate can fail part-way through pass 2, so a half-slid image is
 * never booted) and stores slide 0: the boot goes on unslid. `*outSlide` is always written.
 * Returns BOOT_OK, or the elfLoad() error if that re-load itself fails (the caller must then
 * refuse to boot: `dest` is unusable). `file`/`fileSize` are the whole kernel.elf as elfParse saw
 * it; `dest` is img->span bytes. Serial output only (never the framebuffer). No locks, never
 * sleeps, boot-time only. */
BootStatus loaderKaslrApply(const ElfImage *img, const uint8_t *file, uint64_t fileSize,
                            uint8_t *dest, const uint8_t seed[64], bool kaslrOn,
                            uint64_t *outSlide);

#endif
