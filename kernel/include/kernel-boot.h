/* Accessors for the kernel's own copy of the loader's handoff data (kernel/core/main.c), used by
 * ktests that need to inspect the BootInfo/cmdline/memory-map the kernel actually booted with. */
#ifndef KERNEL_KERNEL_BOOT_H
#define KERNEL_KERNEL_BOOT_H

#include "bootinfo.h"

#include <stdint.h>

/* The kernel's own copy of the loader's BootInfo (kernel .data, `bootInfoCopy` -- M2.3, D-089:
 * before that, this returned the live loader-supplied pointer directly, which stopped being valid
 * once LOADER_RECLAIM is freed). `bi->memMapPhys` points at kernelBootMemMap()'s snapshot, not the
 * original loader array, so every field stays dereferenceable through the kernel's own HHDM
 * mapping for the life of the kernel, including after pmmReclaimLoaderMemory() runs. Only valid
 * from kernelMain's post-validation point on (i.e. from ktests, which run after it). No locks;
 * read-only. */
const BootInfo *kernelBootInfo(void);

/* The kernel's own NUL-terminated copy of the command line. Same availability as
 * kernelBootInfo(). No locks; read-only. */
const char *kernelCmdline(void);

/* The kernel's own pmm-backed copy of the loader's memory map (M2.3, D-089): `*outCount` regions,
 * identical in content to the original loader array but living in memory that survives
 * pmmReclaimLoaderMemory(). Same availability as kernelBootInfo(); `kernelBootInfo()->memMapPhys`
 * (via the HHDM) points at this exact same copy, so callers that already have a BootInfo* rarely
 * need this directly. No locks; read-only. */
const BootMemRegion *kernelBootMemMap(uint32_t *outCount);

/* The physical address of the one BootInfo-sized page pmmReclaimLoaderMemory() deliberately keeps
 * out of the buddy allocator (still PAGE_STATE_RESERVED after reclaim) -- M2.6 reads the (by then
 * consumed) random seed from it and frees it. No locks; read-only. */
uint64_t kernelBootInfoPagePhys(void);

#endif
