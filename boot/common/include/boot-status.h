/* Error codes for boot/common/ code (the ELF64 loader, page-table builder, memory-map
 * conversion, boot.cfg parser). Kept separate from the kernel's own Status (uapi/status.h): this
 * code also builds for the UEFI loader (x86_64-windows target) and for host tests, neither of
 * which links the kernel headers. */
#ifndef BOOT_COMMON_BOOT_STATUS_H
#define BOOT_COMMON_BOOT_STATUS_H

typedef enum {
    BOOT_OK = 0,
    BOOT_ERR_ELF_HEADER = -1,
    BOOT_ERR_ELF_PHDR = -2,
    BOOT_ERR_ELF_SEGMENT = -3,
    BOOT_ERR_ELF_WX = -4,
    BOOT_ERR_ELF_RANGE = -5,
    BOOT_ERR_ELF_ENTRY = -6,
    BOOT_ERR_NO_MEMORY = -7,
    BOOT_ERR_PT_CONFLICT = -8,
    BOOT_ERR_MEMMAP_CAPACITY = -9,
    BOOT_ERR_MEMMAP_OVERLAY = -10,
    BOOT_ERR_CFG = -11,
    /* Deviation from the architect's design (D-061): the design's BootStatus list has no code for
     * "this virtual address isn't mapped", which ptLookup() (paging.c) needs to report distinctly
     * from a real conflict. Added here rather than overloading an unrelated code. */
    BOOT_ERR_NOT_MAPPED = -12,
} BootStatus;

/* Returns a static, human-readable string for `s` (never NULL, even for an unrecognized value).
 * No locks, boot-time/host-test only; pure function. */
const char *bootStatusString(BootStatus s);

#endif
