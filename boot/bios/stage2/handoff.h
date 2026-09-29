/* The BIOS-side kernel-load + page-table/BootInfo/long-mode-entry flow (D-107/D-108/D-111,
 * docs/specs/bios-boot.md), the BIOS counterpart of boot/uefi/handoff.c. */
#ifndef BOOT_BIOS_STAGE2_HANDOFF_H
#define BOOT_BIOS_STAGE2_HANDOFF_H

/* Runs the whole BIOS boot flow: E820 -> the heap, disk probe -> GPT -> FAT32, boot.cfg, VBE, the
 * kernel image, RSDP scan, random seed, the shared page-table/BootInfo build (boothandoff.c), and
 * the jump into long mode. Only returns by halting on a fatal error -- there is no firmware to
 * fall back to and no caller left to report to once this starts touching the disk. */
_Noreturn void handoffRun(void);

#endif
