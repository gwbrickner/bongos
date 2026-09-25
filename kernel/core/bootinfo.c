/* See bootinfo-validate.h. */
#include "bootinfo-validate.h"

#include "sections.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOTINFO_PAGE_SIZE   4096ULL
#define BOOTINFO_KASLR_ALIGN (2ULL * 1024 * 1024)
#define BOOTINFO_MEMMAP_MAX  8192u

Status bootInfoCheckHeader(const BootInfo *bi, const char **why) {
    if (bi->magic != BOOTINFO_MAGIC) {
        *why = "bad magic";
        return STATUS_ERR_INVALID;
    }
    if (bi->version != BOOTINFO_VERSION) {
        *why = "unsupported BootInfo version";
        return STATUS_ERR_INVALID;
    }
    if (bi->size != sizeof(BootInfo)) {
        *why = "BootInfo.size does not match sizeof(BootInfo)";
        return STATUS_ERR_INVALID;
    }
    if (bi->bootMethod != BOOT_METHOD_UEFI && bi->bootMethod != BOOT_METHOD_BIOS) {
        *why = "unknown bootMethod";
        return STATUS_ERR_INVALID;
    }
    if (bi->hhdmBase != BOOTINFO_HHDM_BASE) {
        *why = "hhdmBase does not match BOOTINFO_HHDM_BASE";
        return STATUS_ERR_INVALID;
    }

    if ((bi->kaslrSlide % BOOTINFO_KASLR_ALIGN) != 0) {
        *why = "kaslrSlide is not 2 MiB aligned";
        return STATUS_ERR_INVALID;
    }
    if (bi->kernelVirtBase != (uint64_t)(uintptr_t)kernelImageStart) {
        *why = "kernelVirtBase does not match kernelImageStart";
        return STATUS_ERR_INVALID;
    }
    if (bi->kernelVirtBase - bi->kaslrSlide != BOOTINFO_KERNEL_WINDOW_BASE) {
        *why = "kernelVirtBase - kaslrSlide does not match the kernel link base";
        return STATUS_ERR_INVALID;
    }
    uint64_t expectedSize =
        (uint64_t)(uintptr_t)kernelImageEnd - (uint64_t)(uintptr_t)kernelImageStart;
    if (bi->kernelSize != expectedSize) {
        *why = "kernelSize does not match the linked image size";
        return STATUS_ERR_INVALID;
    }
    if ((bi->kernelPhysBase % BOOTINFO_PAGE_SIZE) != 0) {
        *why = "kernelPhysBase is not 4 KiB aligned";
        return STATUS_ERR_INVALID;
    }

    if (bi->memMapCount < 1 || bi->memMapCount > BOOTINFO_MEMMAP_MAX) {
        *why = "memMapCount is out of range";
        return STATUS_ERR_INVALID;
    }
    if ((bi->memMapPhys % 8) != 0) {
        *why = "memMapPhys is not 8-byte aligned";
        return STATUS_ERR_INVALID;
    }
    uint64_t mapBytes = (uint64_t)bi->memMapCount * sizeof(BootMemRegion);
    if (bi->memMapPhys > BOOTINFO_HHDM_SIZE || mapBytes > BOOTINFO_HHDM_SIZE - bi->memMapPhys) {
        *why = "memory map array does not fit inside the HHDM window";
        return STATUS_ERR_INVALID;
    }
    return STATUS_OK;
}

Status bootMemMapCheck(const BootMemRegion *regions, uint32_t count, const char **why) {
    for (uint32_t i = 0; i < count; i++) {
        const BootMemRegion *r = &regions[i];
        if (r->length == 0) {
            *why = "a memory region has zero length";
            return STATUS_ERR_INVALID;
        }
        if ((r->base % BOOTINFO_PAGE_SIZE) != 0 || (r->length % BOOTINFO_PAGE_SIZE) != 0) {
            *why = "a memory region is not 4 KiB aligned";
            return STATUS_ERR_INVALID;
        }
        uint64_t end = r->base + r->length;
        if (end < r->base) {
            *why = "a memory region overflows";
            return STATUS_ERR_INVALID;
        }
        if (r->type < BOOT_MEM_USABLE || r->type > BOOT_MEM_FRAMEBUFFER) {
            *why = "a memory region has an unknown type";
            return STATUS_ERR_INVALID;
        }
        if (r->reserved != 0) {
            *why = "a memory region's reserved field is nonzero";
            return STATUS_ERR_INVALID;
        }
        if (i > 0) {
            const BootMemRegion *prev = &regions[i - 1];
            if (r->base < prev->base + prev->length) {
                *why = "the memory map is not sorted and non-overlapping";
                return STATUS_ERR_INVALID;
            }
        }
    }
    return STATUS_OK;
}

/* True if [base, base+length) lies entirely inside a single region of `regions` whose type is
 * `type`. A zero-length range is vacuously true (nothing to contain). */
static bool bootRangeInsideType(const BootMemRegion *regions, uint32_t count, uint64_t base,
                                uint64_t length, uint32_t type) {
    if (length == 0) {
        return true;
    }
    uint64_t end = base + length;
    if (end < base) { /* overflow: no real range can wrap the address space */
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].type != type) {
            continue;
        }
        uint64_t regionEnd = regions[i].base + regions[i].length;
        if (regions[i].base <= base && end <= regionEnd) {
            return true;
        }
    }
    return false;
}

/* Finds the single region of `type` that contains the single byte at `base` and returns its end
 * address. Unlike bootRangeInsideType, this doesn't need a length up front -- it's for validating
 * where a variable-length blob (like a NUL-terminated string) *starts* before touching any of its
 * bytes, so the caller then knows how far past `base` it's safe to read. */
static bool bootFindContainingRegionEnd(const BootMemRegion *regions, uint32_t count, uint64_t base,
                                        uint32_t type, uint64_t *outEnd) {
    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].type != type) {
            continue;
        }
        uint64_t regionEnd = regions[i].base + regions[i].length;
        if (regions[i].base <= base && base < regionEnd) {
            *outEnd = regionEnd;
            return true;
        }
    }
    return false;
}

Status bootInfoCheckRefs(const BootInfo *bi, const BootMemRegion *regions, uint32_t count,
                         const char **why) {
    uint64_t mapBytes = (uint64_t)count * sizeof(BootMemRegion);
    if (!bootRangeInsideType(regions, count, bi->memMapPhys, mapBytes, BOOT_MEM_LOADER_RECLAIM)) {
        *why = "the memory map array is not inside a LOADER_RECLAIM region";
        return STATUS_ERR_INVALID;
    }

    uint64_t biPhys = (uint64_t)(uintptr_t)bi - bi->hhdmBase;
    if (!bootRangeInsideType(regions, count, biPhys, sizeof(BootInfo), BOOT_MEM_LOADER_RECLAIM)) {
        *why = "the BootInfo page is not inside a LOADER_RECLAIM region";
        return STATUS_ERR_INVALID;
    }

    if (bi->cmdlinePhys == 0) {
        *why = "cmdlinePhys is zero";
        return STATUS_ERR_INVALID;
    }
    /* Containment is checked *before* any dereference: a corrupted cmdlinePhys could otherwise
     * point at a non-canonical or unmapped address, and scanning it for a NUL terminator first
     * would fault with the null IDT (instant triple fault, no diagnostic). Cap the scan at the
     * containing region's own end rather than a fixed BOOTINFO_CMDLINE_MAX, so it never reads past
     * memory the loader actually claimed for it either. */
    uint64_t cmdlineRegionEnd = 0;
    if (!bootFindContainingRegionEnd(regions, count, bi->cmdlinePhys, BOOT_MEM_LOADER_RECLAIM,
                                     &cmdlineRegionEnd)) {
        *why = "cmdline is not inside a LOADER_RECLAIM region";
        return STATUS_ERR_INVALID;
    }
    uint64_t cmdlineMaxScan = cmdlineRegionEnd - bi->cmdlinePhys;
    if (cmdlineMaxScan > BOOTINFO_CMDLINE_MAX) {
        cmdlineMaxScan = BOOTINFO_CMDLINE_MAX;
    }
    const char *cmdline = (const char *)(uintptr_t)(bi->hhdmBase + bi->cmdlinePhys);
    uint32_t cmdLen = 0;
    bool foundNul = false;
    for (; (uint64_t)cmdLen < cmdlineMaxScan; cmdLen++) {
        if (cmdline[cmdLen] == '\0') {
            foundNul = true;
            break;
        }
    }
    if (!foundNul) {
        *why = "cmdline is not NUL-terminated within its containing region or "
               "BOOTINFO_CMDLINE_MAX bytes";
        return STATUS_ERR_INVALID;
    }

    if (!bootRangeInsideType(regions, count, bi->kernelPhysBase, bi->kernelSize, BOOT_MEM_KERNEL)) {
        *why = "the kernel image is not inside a KERNEL region";
        return STATUS_ERR_INVALID;
    }

    bool haveUsable = false;
    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].type == BOOT_MEM_USABLE) {
            haveUsable = true;
            break;
        }
    }
    if (!haveUsable) {
        *why = "no USABLE region in the memory map";
        return STATUS_ERR_INVALID;
    }

    if (bi->initrdSize != 0 &&
        !bootRangeInsideType(regions, count, bi->initrdPhys, bi->initrdSize, BOOT_MEM_INITRD)) {
        *why = "initrd is not inside an INITRD region";
        return STATUS_ERR_INVALID;
    }

    if (bi->fb.phys != 0) {
        uint64_t fbSize = (uint64_t)bi->fb.pitch * bi->fb.height;
        if (!bootRangeInsideType(regions, count, bi->fb.phys, fbSize, BOOT_MEM_FRAMEBUFFER)) {
            *why = "framebuffer is not inside a FRAMEBUFFER region";
            return STATUS_ERR_INVALID;
        }
        if (bi->fb.reserved[0] != 0 || bi->fb.reserved[1] != 0) {
            *why = "framebuffer reserved bytes are nonzero";
            return STATUS_ERR_INVALID;
        }
    }

    return STATUS_OK;
}

Status bootInfoValidate(const BootInfo *bi, const char **why) {
    if (bi == NULL) {
        *why = "BootInfo pointer is NULL";
        return STATUS_ERR_INVALID;
    }
    uint64_t addr = (uint64_t)(uintptr_t)bi;
    if ((addr & 7) != 0) {
        *why = "BootInfo pointer is not 8-byte aligned";
        return STATUS_ERR_INVALID;
    }
    if (addr < BOOTINFO_HHDM_BASE ||
        addr - BOOTINFO_HHDM_BASE > BOOTINFO_HHDM_SIZE - sizeof(BootInfo)) {
        *why = "BootInfo pointer is outside the HHDM window";
        return STATUS_ERR_INVALID;
    }

    Status st = bootInfoCheckHeader(bi, why);
    if (st != STATUS_OK) {
        return st;
    }

    const BootMemRegion *regions =
        (const BootMemRegion *)(uintptr_t)(bi->hhdmBase + bi->memMapPhys);
    st = bootMemMapCheck(regions, bi->memMapCount, why);
    if (st != STATUS_OK) {
        return st;
    }

    return bootInfoCheckRefs(bi, regions, bi->memMapCount, why);
}

const char *bootMemTypeName(uint32_t type) {
    switch (type) {
        case BOOT_MEM_USABLE:
            return "USABLE";
        case BOOT_MEM_RESERVED:
            return "RESERVED";
        case BOOT_MEM_ACPI_RECLAIM:
            return "ACPI_RECLAIM";
        case BOOT_MEM_ACPI_NVS:
            return "ACPI_NVS";
        case BOOT_MEM_BAD:
            return "BAD";
        case BOOT_MEM_LOADER_RECLAIM:
            return "LOADER_RECLAIM";
        case BOOT_MEM_KERNEL:
            return "KERNEL";
        case BOOT_MEM_INITRD:
            return "INITRD";
        case BOOT_MEM_FRAMEBUFFER:
            return "FRAMEBUFFER";
        default:
            return "UNKNOWN";
    }
}
