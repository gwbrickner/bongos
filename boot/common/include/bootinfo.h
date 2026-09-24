/* BootInfo: the boot handoff ABI (ARCHITECTURE §5.3), shared by boot/uefi, boot/bios (M2.5), and
 * the kernel. Field order and sizes are exactly ARCHITECTURE §5.3's struct; do not reorder fields
 * or add padding without bumping BOOTINFO_VERSION and updating that section in the same PR.
 * All addresses in this struct are physical; the kernel reaches them through the HHDM. */
#ifndef BOOT_COMMON_BOOTINFO_H
#define BOOT_COMMON_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC   0x544F4F42474E4F42ULL /* "BONGBOOT" */
#define BOOTINFO_VERSION 1u                    /* bump on any layout change */

/* D-059: the handoff/HHDM contract these macros encode. hhdmBase/hhdmSize match ARCHITECTURE
 * §6.1's HHDM range; the kernel window is §6.1's "kernel image" range. */
#define BOOTINFO_HHDM_BASE          0xFFFF800000000000ULL
#define BOOTINFO_HHDM_SIZE          0x0000400000000000ULL /* 64 TiB */
#define BOOTINFO_KERNEL_WINDOW_BASE 0xFFFFFFFF80000000ULL
#define BOOTINFO_KERNEL_WINDOW_END  0xFFFFFFFFA0000000ULL
#define BOOTINFO_CMDLINE_MAX        4096u /* bytes, including the NUL */

typedef enum { BOOT_METHOD_UEFI = 1, BOOT_METHOD_BIOS = 2 } BootMethod;

typedef enum {
    BOOT_MEM_USABLE = 1,
    BOOT_MEM_RESERVED,
    BOOT_MEM_ACPI_RECLAIM,
    BOOT_MEM_ACPI_NVS,
    BOOT_MEM_BAD,
    BOOT_MEM_LOADER_RECLAIM, /* BootInfo, loader page tables, boot stack */
    BOOT_MEM_KERNEL,
    BOOT_MEM_INITRD,
    BOOT_MEM_FRAMEBUFFER
} BootMemType;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} BootMemRegion;

typedef struct {
    uint64_t phys;
    uint32_t width, height, pitch, bpp;
    uint8_t redShift, redSize, greenShift, greenSize, blueShift, blueSize, reserved[2];
} BootFramebuffer;

typedef struct BootInfo {
    uint64_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t bootMethod;
    uint32_t memMapCount;
    uint64_t memMapPhys; /* BootMemRegion[memMapCount], sorted, non-overlapping */
    BootFramebuffer fb;
    uint64_t rsdpPhys; /* ACPI RSDP */
    uint64_t kernelPhysBase, kernelVirtBase, kernelSize, kaslrSlide;
    uint64_t initrdPhys, initrdSize;
    uint64_t cmdlinePhys;        /* NUL-terminated */
    uint64_t hhdmBase;           /* virtual base of the direct physical map */
    uint64_t loaderTsc;          /* TSC when the loader started (boot-time stats) */
    uint64_t efiSystemTablePhys; /* 0 on BIOS; the kernel does not use runtime services in v1 */
    uint8_t bootDiskGuid[16], bootPartGuid[16]; /* so the kernel can find its disk */
    uint8_t randomSeed[64];                     /* EFI_RNG / RDSEED / RDRAND / TSC jitter */
} BootInfo;

_Static_assert(sizeof(BootMemRegion) == 24, "BootMemRegion must be 24 bytes (ARCHITECTURE §5.3)");
_Static_assert(sizeof(BootFramebuffer) == 32,
               "BootFramebuffer must be 32 bytes (ARCHITECTURE §5.3)");
_Static_assert(sizeof(BootInfo) == 248, "BootInfo must be 248 bytes (ARCHITECTURE §5.3)");

#endif
