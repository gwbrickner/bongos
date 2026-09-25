/* The main boot flow after early setup (ARCHITECTURE §5.5 steps 2-10, D-059/D-066): reads
 * boot.cfg + kernel.elf, builds the page tables and BootInfo, exits boot services, and jumps to
 * the kernel. */
#ifndef LOADER_HANDOFF_H
#define LOADER_HANDOFF_H

#include "include/efi/efi.h"

/* Runs to completion or returns an EFI_STATUS describing why it couldn't (the caller logs it and
 * halts); on success this never returns -- the last thing it does is jump into the kernel.
 * `loaderTsc` is the RDTSC value from the very top of efiMain (ARCHITECTURE §5.5 step 1),
 * threaded through rather than re-read here so BootInfo.loaderTsc reflects the loader's true
 * start time. */
EFI_STATUS handoffRun(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE *st, uint64_t loaderTsc);

#endif
