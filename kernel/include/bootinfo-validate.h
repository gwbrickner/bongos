/* Kernel-side BootInfo validation (ARCHITECTURE §5.3, D-064). Named differently from the shared
 * ABI header it wraps (boot/common/include/bootinfo.h -- also literally "bootinfo.h") so the two
 * can both be on the include path without one shadowing the other. */
#ifndef KERNEL_BOOTINFO_VALIDATE_H
#define KERNEL_BOOTINFO_VALIDATE_H

#include "bootinfo.h" /* boot/common/include/bootinfo.h, via kernel.mk's include path */
#include "uapi/status.h"

/* Validates `bi` end to end: the pointer itself (NULL/alignment/HHDM-bounds, checked before any
 * dereference), the header fields, the memory-map array, and every cross-reference the map array
 * makes (kernel image, cmdline, BootInfo's own page, initrd, framebuffer). On the first failing
 * check, returns STATUS_ERR_INVALID and sets `*why` to a static, human-readable string; the
 * caller (kernelMain) panics with it. No locks, boot-time only; read-only (never modifies `bi` or
 * the memory it points to). */
Status bootInfoValidate(const BootInfo *bi, const char **why);

/* The header-only subset of bootInfoValidate()'s checks (magic/version/size/bootMethod/hhdmBase,
 * plus the kernel-image and memory-map-array fields) -- split out so ktests can feed it corrupted
 * copies of just the header without needing a full memory map. Same contract as
 * bootInfoValidate(). */
Status bootInfoCheckHeader(const BootInfo *bi, const char **why);

/* Validates `regions` (`count` entries) in isolation: page alignment, non-zero length, no
 * overflow, a known BootMemType, a zero `reserved` field, and that the array is sorted and
 * non-overlapping. Same contract as bootInfoValidate(). */
Status bootMemMapCheck(const BootMemRegion *regions, uint32_t count, const char **why);

/* Validates every cross-reference `bi` makes into `regions` (`count` entries): the map array
 * itself, BootInfo's own page, and the cmdline all lie inside LOADER_RECLAIM regions; the kernel
 * image lies inside a KERNEL region; at least one USABLE region exists; the cmdline is
 * NUL-terminated within BOOTINFO_CMDLINE_MAX bytes; and, if present, the initrd/framebuffer lie
 * inside their own region types. Same contract as bootInfoValidate(). */
Status bootInfoCheckRefs(const BootInfo *bi, const BootMemRegion *regions, uint32_t count,
                         const char **why);

/* A short uppercase name for a BootMemType ("USABLE", "RESERVED", ...), or "UNKNOWN" for a value
 * outside [1,9]. Used for the boot-time memory-map summary and by klog_test.c. No locks; pure. */
const char *bootMemTypeName(uint32_t type);

#endif
