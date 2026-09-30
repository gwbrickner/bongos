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
    /* Addition beyond the architect's original design: the design's BootStatus list has no code
     * for "this virtual address isn't mapped", which ptLookup() (paging.c) needs to report
     * distinctly from a real conflict. Added here rather than overloading an unrelated code. */
    BOOT_ERR_NOT_MAPPED = -12,
    /* Also an addition: ptMapRange() needs to report a misaligned va/pa/size distinctly from a
     * real conflict (mapping over an existing entry). */
    BOOT_ERR_PT_UNALIGNED = -13,
    /* M1.4 addition: fbtext.c's geometry validation (bpp/pitch/shift-size sanity, D-068/D-069)
     * needs a code distinct from BOOT_ERR_CFG (a boot.cfg problem, not a framebuffer one). */
    BOOT_ERR_FB_UNSUPPORTED = -14,
    /* M2.5 (BIOS loader, D-105/D-106) additions: a block-device read failed; a GPT header/array
     * failed validation on both the primary and backup; a FAT32 volume/directory/file structure
     * failed validation or a chain walk hit something invalid; a lookup (partition, file) found
     * nothing matching; a destination buffer was too small for what was being read into it. */
    BOOT_ERR_IO = -15,
    BOOT_ERR_GPT = -16,
    BOOT_ERR_FAT = -17,
    BOOT_ERR_NOT_FOUND = -18,
    BOOT_ERR_TOO_LARGE = -19,
    /* M2.6 (KASLR, D-120) addition: elfRelocate() found a relocation table it cannot apply
     * safely (unsupported type, out-of-image location, malformed section/symbol table, no
     * relocations in code, overflow). Distinct from BOOT_ERR_ELF_SEGMENT: the segments were fine.
     */
    BOOT_ERR_ELF_RELOC = -20,
} BootStatus;

/* Returns a static, human-readable string for `s` (never NULL, even for an unrecognized value).
 * No locks, boot-time/host-test only; pure function. */
const char *bootStatusString(BootStatus s);

#endif
