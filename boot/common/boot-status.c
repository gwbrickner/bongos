#include "include/boot-status.h"

const char *bootStatusString(BootStatus s) {
    switch (s) {
        case BOOT_OK:
            return "ok";
        case BOOT_ERR_ELF_HEADER:
            return "invalid ELF header";
        case BOOT_ERR_ELF_PHDR:
            return "invalid ELF program header table";
        case BOOT_ERR_ELF_SEGMENT:
            return "invalid ELF segment";
        case BOOT_ERR_ELF_WX:
            return "ELF segment is both writable and executable";
        case BOOT_ERR_ELF_RANGE:
            return "ELF segment outside the kernel window";
        case BOOT_ERR_ELF_ENTRY:
            return "ELF entry point outside an executable segment";
        case BOOT_ERR_NO_MEMORY:
            return "out of page-table pool memory";
        case BOOT_ERR_PT_CONFLICT:
            return "conflicting page-table mapping";
        case BOOT_ERR_MEMMAP_CAPACITY:
            return "memory map output capacity exceeded";
        case BOOT_ERR_MEMMAP_OVERLAY:
            return "loader allocation outside its backing EFI memory type";
        case BOOT_ERR_CFG:
            return "invalid boot.cfg";
        case BOOT_ERR_NOT_MAPPED:
            return "virtual address not mapped";
        case BOOT_ERR_PT_UNALIGNED:
            return "page-table mapping request not 4 KiB aligned";
        case BOOT_ERR_FB_UNSUPPORTED:
            return "unsupported framebuffer geometry";
        default:
            return "unknown boot status";
    }
}
