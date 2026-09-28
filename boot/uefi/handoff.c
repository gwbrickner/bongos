/* See handoff.h. Implements ARCHITECTURE §5.5 steps 2-11 (D-059/D-060/D-066/D-068): CPU checks,
 * reading boot.cfg, GOP mode selection and the boot menu (boot/uefi/gop.c, menu.c), reading
 * kernel.elf, the page-table build including the framebuffer HHDM mapping (boot/common/paging.c),
 * the BootInfo build, the ExitBootServices retry loop, and the final jump through
 * boot/uefi/trampoline.asm. The page-table/BootInfo-build core and the CPU/MSR/RDTSC/RDSEED
 * primitives are shared with BIOS stage2 (D-104/D-108: boot/common/hw/cpu.c,
 * boot/common/boothandoff.c) -- this file gathers EFI-specific inputs (GetMemoryMap,
 * EFI_RNG_PROTOCOL, EFI config tables, ExitBootServices) and calls into those.
 *
 * Simplifications versus the full M1.3 design (noted in the M1.3 milestone log, not blocking the
 * Done-when check): the page-table pool and the BootInfo memory-map array are fixed, generously
 * sized allocations rather than computed from the exact formula in the design doc (safe for
 * anything up to a few hundred EFI memory-map descriptors, comfortably true under QEMU); the
 * loader does not re-verify that the final (post-EBS) memory map's RAM descriptors are a subset
 * of the pre-EBS run set it already mapped, nor call memMapCheckOverlay() at runtime (that
 * function exists and is host-tested, for a later milestone to wire in). */
#include "handoff.h"

#include "branding.h"
#include "file.h"
#include "gop.h"
#include "include/efi/guids.h"
#include "loader-cpu.h"
#include "loader-serial.h"
#include "menu.h"

#include "bootcfg.h"
#include "boothandoff.h"
#include "bootmem.h"
#include "elf64.h"
#include "fbtext.h"
#include "memmap.h"
#include "paging.h"

#include <stdbool.h>

/* boot/common/hw/libc-shim.c defines this (D-065: this freestanding target has no libc header to
 * declare it), matching this exact signature. */
extern int memcmp(const void *a, const void *b, size_t n);

/* The page-table pool size, boot stack size, and BootInfo memory-map array capacity now live in
 * boothandoff.h as BOOT_HANDOFF_PT_POOL_PAGES/BOOT_HANDOFF_BOOT_STACK_PAGES/
 * BOOT_HANDOFF_MEMMAP_CAP/BOOT_HANDOFF_MEMMAP_CAP_PAGES/BOOT_HANDOFF_PAGE_SIZE (D-108), shared
 * with BIOS stage2's own handoff.c so the two loaders' copies of these constants can't drift. */
#define HANDOFF_MAX_INPUTS 512u

static void handoffHalt(const char *msg) {
    loaderSerialWriteString(msg);
    for (;;) {
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}

/* EFI_RNG_PROTOCOL if present, else loaderCpuRandomFill()'s RDSEED/RDRAND-then-RDTSC fallback
 * (ARCHITECTURE §5.5 step 7). */
static void handoffGatherRandomSeed(EFI_SYSTEM_TABLE *st, uint64_t seed[8]) {
    EFI_RNG_PROTOCOL *rng = NULL;
    if (!EFI_ERROR(st->BootServices->LocateProtocol((EFI_GUID *)&gEfiRngProtocolGuid, NULL,
                                                    (VOID **)&rng)) &&
        !EFI_ERROR(rng->GetRNG(rng, NULL, 64, (UINT8 *)seed))) {
        return;
    }
    if (!loaderCpuHasRdseed() && !loaderCpuHasRdrand()) {
        loaderSerialWriteString(
            "loader: no EFI_RNG_PROTOCOL, RDSEED, or RDRAND; seeding from TSC jitter only\n");
    }
    loaderCpuRandomFill(seed);
}

static uint64_t handoffFindRsdp(EFI_SYSTEM_TABLE *st) {
    uint64_t acpi20 = 0, acpi10 = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *e = &st->ConfigurationTable[i];
        if (memcmp(&e->VendorGuid, &gEfiAcpi20TableGuid, sizeof(EFI_GUID)) == 0) {
            acpi20 = (uint64_t)(uintptr_t)e->VendorTable;
        } else if (memcmp(&e->VendorGuid, &gEfiAcpi10TableGuid, sizeof(EFI_GUID)) == 0) {
            acpi10 = (uint64_t)(uintptr_t)e->VendorTable;
        }
    }
    return acpi20 != 0 ? acpi20 : acpi10;
}

/* The RAM types ARCHITECTURE §5's HHDM coverage rule maps (D-059): every RAM-backed EFI type,
 * including Runtime (a runtime region freed before EBS must not become an unmapped USABLE
 * region). MMIO/reserved/persistent/unaccepted types are deliberately excluded. */
static bool handoffIsHhdmType(UINT32 efiType) {
    switch (efiType) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 9:
        case 10:
            return true;
        default:
            return false;
    }
}

/* The classic GetMemoryMap/AllocatePool-retry pattern: not used for the final, pre-EBS map (which
 * must not allocate between GetMemoryMap and ExitBootServices), only for the earlier "pre-map"
 * that HHDM run-building needs. */
static EFI_STATUS handoffGetMemoryMapPool(EFI_SYSTEM_TABLE *st, VOID **outBuf, UINTN *outSize,
                                          UINTN *outDescSize, UINT32 *outDescVer) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN size = 0;
    UINTN mapKey = 0;
    EFI_STATUS status = bs->GetMemoryMap(&size, NULL, &mapKey, outDescSize, outDescVer);
    if (status != EFI_BUFFER_TOO_SMALL) {
        return EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;
    }

    for (int attempt = 0; attempt < 8; attempt++) {
        size += (*outDescSize) * 8; /* slack: AllocatePool itself can grow the map by one entry */
        VOID *buf = NULL;
        status = bs->AllocatePool(EfiLoaderData, size, &buf);
        if (EFI_ERROR(status)) {
            return status;
        }
        UINTN callSize = size;
        status = bs->GetMemoryMap(&callSize, (EFI_MEMORY_DESCRIPTOR *)buf, &mapKey, outDescSize,
                                  outDescVer);
        if (status == EFI_BUFFER_TOO_SMALL) {
            bs->FreePool(buf);
            size = callSize;
            continue;
        }
        if (EFI_ERROR(status)) {
            bs->FreePool(buf);
            return status;
        }
        *outBuf = buf;
        *outSize = callSize;
        return EFI_SUCCESS;
    }
    return EFI_OUT_OF_RESOURCES;
}

extern void loaderTrampoline(uint64_t cr3, uint64_t bootInfoVa, uint64_t stackTopVa,
                             uint64_t entryVa);

/* Static rather than on the stack: several KiB apiece, and this function's stack usage otherwise
 * competes with the loader's small default stack. */
static MemMapInput preRunsInput[HANDOFF_MAX_INPUTS];
static BootMemRegion hhdmRuns[HANDOFF_MAX_INPUTS * 2];
static uint64_t hhdmScratch[HANDOFF_MAX_INPUTS * 2];
static MemMapInput finalInputs[HANDOFF_MAX_INPUTS];
static uint64_t finalScratch[HANDOFF_MAX_INPUTS * 2];

EFI_STATUS handoffRun(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE *st, uint64_t loaderTsc) {
    EFI_BOOT_SERVICES *bs = st->BootServices;

    bool has1G = false;
    if (!loaderCpuCheckLongModeFeatures(&has1G)) {
        loaderSerialWriteString("loader: CPU lacks NX; refusing to boot\n");
        return EFI_UNSUPPORTED;
    }
    if (loaderCpuLa57Enabled()) {
        loaderSerialWriteString(
            "loader: CR4.LA57 (5-level paging) is enabled; this loader only builds 4-level page "
            "tables, refusing to boot\n");
        return EFI_UNSUPPORTED;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS status = loaderOpenVolume(st, imageHandle, &root);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: failed to open the ESP volume\n");
        return status;
    }

    /* D-067's two-phase API: bootCfgParse() builds spans + the parsed [entry] sections, then
     * bootCfgResolveEntry() copies one entry's effective (inherited) values out. A missing
     * boot.cfg is treated as an empty one (bootCfgParse("", 0, ...) synthesizes the same
     * single-implicit-entry, all-defaults BootCfg a hand-rolled "file not found" path would have
     * built, so there's only one code path from here on regardless of which happened). */
    uint8_t *cfgBuf = NULL;
    uint64_t cfgSize = 0;
    static const char emptyCfgText[] = "";
    const char *cfgText = emptyCfgText;
    uint64_t cfgTextLen = 0;
    status = loaderReadFile(st, root, (CHAR16 *)L"\\bong\\boot.cfg", &cfgBuf, &cfgSize);
    if (!EFI_ERROR(status)) {
        cfgText = (const char *)cfgBuf;
        cfgTextLen = cfgSize;
    } else {
        loaderSerialWriteString("loader: boot.cfg not found; using defaults\n");
    }

    BootCfg cfg;
    BootStatus bst = bootCfgParse(cfgText, cfgTextLen, &cfg);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: boot.cfg:");
        loaderSerialWriteUint(cfg.errorLine);
        loaderSerialWriteString(": ");
        loaderSerialWriteString(cfg.errorReason != NULL ? cfg.errorReason : bootStatusString(bst));
        loaderSerialWriteString("\n");
        if (cfgBuf != NULL) {
            bs->FreePool(cfgBuf);
        }
        return EFI_INVALID_PARAMETER;
    }

    /* ARCHITECTURE §5.5 steps 2-4 (D-068): pick the GOP mode using the global resolution before
     * showing anything (the menu needs a framebuffer), show the menu if timeout > 0, then pick
     * the mode again only if the selected entry's resolution differs from the global one. After
     * the first SetMode, ConOut is never touched again (GraphicsConsole doesn't know the mode
     * changed and would blit at stale geometry) -- EnableCursor(FALSE) is the very last ConOut
     * call in this function. */
    LoaderGop gop;
    loaderGopFind(st, &gop);
    if (gop.gop != NULL) {
        st->ConOut->EnableCursor(st->ConOut, FALSE);
    }
    uint32_t globalResWidth =
        (cfg.global.setMask & BOOT_CFG_HAS_RESOLUTION) ? cfg.global.resWidth : 0;
    uint32_t globalResHeight =
        (cfg.global.setMask & BOOT_CFG_HAS_RESOLUTION) ? cfg.global.resHeight : 0;
    BootFramebuffer fb;
    status = loaderGopSetMode(st, &gop, globalResWidth, globalResHeight, &fb);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: GOP mode selection failed; continuing without a "
                                "framebuffer\n");
        bootMemset(&fb, 0, sizeof(fb));
    }

    uint32_t selectedIndex = cfg.defaultIndex;
    if (cfg.timeoutSec > 0) {
        /* ARCHITECTURE §5.2: the menu runs on screen *and* serial whenever there's a timeout to
         * show one for -- never skipped outright just because no framebuffer came up. `fxPtr`
         * stays NULL (serial-only) unless a usable framebuffer geometry is actually available;
         * loaderMenuRun()/the shared menu-ui drawing all tolerate a NULL fx (D-071). */
        BootFbText fx;
        BootFbText *fxPtr = NULL;
        if (fb.phys != 0) {
            /* Pre-ExitBootServices, every physical address the firmware hands out (including a
             * GOP framebuffer BAR) is still directly usable as a pointer -- the same trick
             * bootPhysToPtr() documents for BootInfo/the kernel image. This is not the kernel's
             * HHDM mapping (built later, after the menu, once the final framebuffer is known). */
            BootStatus fxSt = fbTextInit(&fx, (uint8_t *)bootPhysToPtr(fb.phys), fb.width,
                                         fb.height, fb.pitch, fb.redShift, fb.redSize,
                                         fb.greenShift, fb.greenSize, fb.blueShift, fb.blueSize);
            if (fxSt == BOOT_OK) {
                fxPtr = &fx;
            } else {
                loaderSerialWriteString(
                    "loader: framebuffer geometry unusable for the menu; continuing serial-only\n");
            }
        } else {
            loaderSerialWriteString("loader: no framebuffer available; menu is serial-only\n");
        }
        selectedIndex = loaderMenuRun(st, cfgText, cfgTextLen, &cfg, fxPtr);
    }

    BootCfgEntry entry;
    bst = bootCfgResolveEntry(cfgText, cfgTextLen, &cfg, selectedIndex, &entry);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: boot.cfg: failed to resolve the selected entry\n");
        if (cfgBuf != NULL) {
            bs->FreePool(cfgBuf);
        }
        return EFI_INVALID_PARAMETER;
    }

    if (entry.resWidth != globalResWidth || entry.resHeight != globalResHeight) {
        status = loaderGopSetMode(st, &gop, entry.resWidth, entry.resHeight, &fb);
        if (EFI_ERROR(status)) {
            loaderSerialWriteString("loader: GOP mode re-selection for the chosen entry failed; "
                                    "continuing without a framebuffer\n");
            bootMemset(&fb, 0, sizeof(fb));
        }
    }

    if (cfgBuf != NULL) {
        bs->FreePool(cfgBuf);
    }

    CHAR16 kernelPathW[BOOT_CFG_PATH_MAX];
    loaderAsciiPathToWide(entry.kernel, kernelPathW, BOOT_CFG_PATH_MAX);

    uint8_t *kernelBuf = NULL;
    uint64_t kernelSize = 0;
    status = loaderReadFile(st, root, kernelPathW, &kernelBuf, &kernelSize);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: failed to read the kernel image\n");
        return status;
    }

    ElfImage elfImage;
    bst = elfParse(kernelBuf, kernelSize, &elfImage);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: kernel.elf: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        return EFI_LOAD_ERROR;
    }

    BootAllocList allocs;
    bootMemset(&allocs, 0, sizeof(allocs));

    EFI_PHYSICAL_ADDRESS kernelPhys = 0;
    UINTN kernelPages = (UINTN)(elfImage.span / BOOT_HANDOFF_PAGE_SIZE);
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, kernelPages, &kernelPhys);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the kernel image\n");
        return status;
    }
    bst = elfLoad(&elfImage, kernelBuf, (uint8_t *)(uintptr_t)kernelPhys);
    bs->FreePool(kernelBuf);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: kernel.elf: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        return EFI_LOAD_ERROR;
    }
    bootAllocAdd(&allocs, (uint64_t)kernelPhys, kernelPages, BOOT_MEM_KERNEL);

    EFI_PHYSICAL_ADDRESS stackPhys = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, BOOT_HANDOFF_BOOT_STACK_PAGES,
                               &stackPhys);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the boot stack\n");
        return status;
    }
    bootAllocAdd(&allocs, (uint64_t)stackPhys, BOOT_HANDOFF_BOOT_STACK_PAGES,
                 BOOT_MEM_LOADER_RECLAIM);

    uint64_t rsdpPhys = handoffFindRsdp(st);
    uint64_t randomSeed[8];
    handoffGatherRandomSeed(st, randomSeed);

    /* Pre-map: the EFI memory map as it stands right now, used only to size the HHDM run set. */
    VOID *preMapBuf = NULL;
    UINTN preMapSize = 0, preDescSize = 0;
    UINT32 preDescVer = 0;
    status = handoffGetMemoryMapPool(st, &preMapBuf, &preMapSize, &preDescSize, &preDescVer);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: GetMemoryMap (pre-map) failed\n");
        return status;
    }
    uint32_t nRunsInput = 0;
    UINTN nPreDesc = preMapSize / preDescSize;
    for (UINTN i = 0; i < nPreDesc; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)preMapBuf + i * preDescSize);
        if (handoffIsHhdmType(d->Type)) {
            if (nRunsInput >= HANDOFF_MAX_INPUTS) {
                /* Fail loudly rather than silently dropping descriptors: a truncated pre-map
                 * would just make the HHDM run set too small (safe but a smaller HHDM than
                 * intended), but we'd rather flag a memory map this pathological than guess. */
                bs->FreePool(preMapBuf);
                loaderSerialWriteString(
                    "loader: EFI memory map has more usable descriptors than HANDOFF_MAX_INPUTS; "
                    "refusing to boot\n");
                return EFI_OUT_OF_RESOURCES;
            }
            preRunsInput[nRunsInput].base = d->PhysicalStart;
            preRunsInput[nRunsInput].length = d->NumberOfPages * BOOT_HANDOFF_PAGE_SIZE;
            preRunsInput[nRunsInput].type = BOOT_MEM_USABLE; /* rank is irrelevant: one type in */
            nRunsInput++;
        }
    }
    bs->FreePool(preMapBuf);

    uint32_t nHhdmRuns = 0;
    BootStatus mmst = memMapNormalize(preRunsInput, nRunsInput, hhdmRuns, HANDOFF_MAX_INPUTS * 2,
                                      &nHhdmRuns, hhdmScratch);
    if (mmst != BOOT_OK) {
        loaderSerialWriteString("loader: HHDM run set: ");
        loaderSerialWriteString(bootStatusString(mmst));
        loaderSerialWriteString("\n");
        return EFI_OUT_OF_RESOURCES;
    }

    EFI_PHYSICAL_ADDRESS handoffPhys = 0;
    UINTN handoffPages = 2 + BOOT_HANDOFF_MEMMAP_CAP_PAGES;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, handoffPages, &handoffPhys);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the handoff block\n");
        return status;
    }
    bootAllocAdd(&allocs, (uint64_t)handoffPhys, handoffPages, BOOT_MEM_LOADER_RECLAIM);
    uint64_t bootInfoPhys = (uint64_t)handoffPhys;
    uint64_t cmdlinePhys = bootInfoPhys + BOOT_HANDOFF_PAGE_SIZE;
    uint64_t memMapArrayPhys = cmdlinePhys + BOOT_HANDOFF_PAGE_SIZE;

    EFI_PHYSICAL_ADDRESS poolPhys = 0;
    status =
        bs->AllocatePages(AllocateAnyPages, EfiLoaderData, BOOT_HANDOFF_PT_POOL_PAGES, &poolPhys);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the page-table pool\n");
        return status;
    }
    bootAllocAdd(&allocs, (uint64_t)poolPhys, BOOT_HANDOFF_PT_POOL_PAGES, BOOT_MEM_LOADER_RECLAIM);

    uint64_t trampPhys =
        bootAlignDown((uint64_t)(uintptr_t)&loaderTrampoline, BOOT_HANDOFF_PAGE_SIZE);
    bootAllocAdd(&allocs, trampPhys, 1, BOOT_MEM_LOADER_RECLAIM);

    PtBuilder pt;
    bst = ptInit(&pt, (uint64_t)poolPhys, BOOT_HANDOFF_PT_POOL_PAGES, has1G);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: page-table init failed\n");
        return EFI_OUT_OF_RESOURCES;
    }

    BootPtPlan plan = {
        .hhdmRuns = hhdmRuns,
        .hhdmRunCount = nHhdmRuns,
        .elfImage = &elfImage,
        .kernelPhys = (uint64_t)kernelPhys,
        .trampPhys = trampPhys,
        .patEntry2Uncacheable = loaderCpuPatEntry2Uncacheable(),
    };
    const char *fbNote = NULL;
    bst = bootHandoffMapAll(&pt, &plan, &fb, &allocs, &fbNote);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: page-table build failed: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        return EFI_OUT_OF_RESOURCES;
    }
    if (fbNote != NULL) {
        loaderSerialWriteString("loader: framebuffer unusable for handoff: ");
        loaderSerialWriteString(fbNote);
        loaderSerialWriteString("\n");
    }

    /* Self-check (ARCHITECTURE §5.5 step 10): every mapping the trampoline and the kernel's first
     * instructions depend on actually resolves the way it should. */
    uint64_t entryVa = elfImage.entry;
    uint64_t stackTopVa = BOOTINFO_HHDM_BASE + (uint64_t)stackPhys +
                          (uint64_t)BOOT_HANDOFF_BOOT_STACK_PAGES * BOOT_HANDOFF_PAGE_SIZE;
    uint64_t bootInfoVa = BOOTINFO_HHDM_BASE + bootInfoPhys;
    uint64_t cmdlineVa = BOOTINFO_HHDM_BASE + cmdlinePhys;
    uint64_t memMapVa = BOOTINFO_HHDM_BASE + memMapArrayPhys;
    if (!bootHandoffSelfCheck(&pt, entryVa, stackTopVa, bootInfoVa, cmdlineVa, memMapVa, trampPhys,
                              &fb)) {
        loaderSerialWriteString("loader: page-table self-check failed\n");
        return EFI_DEVICE_ERROR;
    }

    BootInfo *bi = (BootInfo *)(uintptr_t)bootInfoPhys;
    BootHandoffFields fields = {
        .bootMethod = BOOT_METHOD_UEFI,
        .fb = fb,
        .rsdpPhys = rsdpPhys,
        .kernelPhys = (uint64_t)kernelPhys,
        .kernelVirtBase = elfImage.linkBase,
        .kernelSize = elfImage.span,
        .cmdlinePhys = cmdlinePhys,
        .hhdmBase = BOOTINFO_HHDM_BASE,
        .loaderTsc = loaderTsc,
        .efiSystemTablePhys = (uint64_t)(uintptr_t)st,
        .randomSeed = (const uint8_t *)randomSeed,
    };
    bootHandoffFillInfo(bi, &fields);
    bi->memMapPhys = memMapArrayPhys;
    /* Wipe the loader's stack-local copy now that it's in the BootInfo page: otherwise it lingers
     * in BootServicesData memory, which becomes plain USABLE after ExitBootServices and would be
     * recoverable by any later kernel code that walks USABLE memory. volatile so this store isn't
     * optimized away as a dead write to a local about to go out of scope. Note: the *original*
     * BootInfo page's seed still needs a proper wipe once a real consumer (M2.1's CSPRNG init)
     * exists and before LOADER_RECLAIM memory is ever freed -- that's a forward-reference for a
     * later milestone, not something this one solves. */
    for (int i = 0; i < 8; i++) {
        *(volatile uint64_t *)&randomSeed[i] = 0;
    }

    /* entry.cmdline is already NUL-terminated and within BOOTINFO_CMDLINE_MAX by construction
     * (bootCfgResolveEntry's cfgCopySpan never overflows its destination), so this is a plain
     * copy, not a second truncation pass -- bootCfgResolveEntry is what actually detects
     * truncation, in entry.cmdlineTruncated. */
    char *cmdlineDst = (char *)(uintptr_t)cmdlinePhys;
    uint32_t cmdLen = 0;
    while (entry.cmdline[cmdLen] != '\0') {
        cmdLen++;
    }
    bootMemcpy(cmdlineDst, entry.cmdline, (uint64_t)cmdLen + 1);
    if (entry.cmdlineTruncated) {
        loaderSerialWriteString("loader: cmdline truncated to fit BOOTINFO_CMDLINE_MAX\n");
    }

    /* No ConOut call here (D-068): GraphicsConsole doesn't know about a mode change from a direct
     * SetMode, and would blit this at stale geometry over the menu/framebuffer. EnableCursor
     * above (before the first SetMode) is the last ConOut call this loader ever makes. */
    loaderSerialWriteString("loader: exiting boot services\n");

    UINTN finalMapCap = (nPreDesc + 64) * preDescSize;
    VOID *finalMapBuf = NULL;
    status = bs->AllocatePool(EfiLoaderData, finalMapCap, &finalMapBuf);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the final memory map\n");
        return status;
    }

    UINTN finalMapKey = 0, finalDescSize = preDescSize, nFinalDesc = 0;
    UINT32 finalDescVer = preDescVer;
    bool exited = false;
    for (int attempt = 0; attempt < 8 && !exited; attempt++) {
        UINTN size = finalMapCap;
        EFI_STATUS mapStatus = bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)finalMapBuf,
                                                &finalMapKey, &finalDescSize, &finalDescVer);
        if (EFI_ERROR(mapStatus)) {
            loaderSerialWriteString("loader: GetMemoryMap (final) failed\n");
            return mapStatus;
        }
        nFinalDesc = size / finalDescSize;

        EFI_STATUS ebsStatus = bs->ExitBootServices(imageHandle, finalMapKey);
        if (!EFI_ERROR(ebsStatus)) {
            exited = true;
        } else if (ebsStatus != EFI_INVALID_PARAMETER) {
            loaderSerialWriteString("loader: ExitBootServices failed\n");
            return ebsStatus;
        }
        /* EFI_INVALID_PARAMETER: the map key went stale; loop and fetch a fresh map. No
         * allocation, ConOut, or other Boot Service call happens between here and the retry. */
    }
    if (!exited) {
        loaderSerialWriteString("loader: ExitBootServices did not succeed after 8 attempts\n");
        return EFI_ABORTED;
    }

    __asm__ volatile("cli");
    loaderCpuSetEferNxe();

    /* We are past ExitBootServices: ConOut and every other Boot Service are gone, and there is no
     * retrying from scratch. A truncated array here would silently drop the overlays appended
     * below (KERNEL, LOADER_RECLAIM for page tables/stack/BootInfo) *first*, since they're added
     * after the raw descriptors -- meaning the kernel image and page tables could show up as plain
     * USABLE memory to a later milestone's allocator. Check the total fits before writing anything,
     * and halt loudly (the only diagnostic left is raw serial) rather than let that happen. */
    if ((uint64_t)nFinalDesc + allocs.count > HANDOFF_MAX_INPUTS) {
        handoffHalt("loader: final memory map + overlay count exceeds HANDOFF_MAX_INPUTS; "
                    "halting\n");
    }
    uint32_t nFinalInputs = 0;
    for (UINTN i = 0; i < nFinalDesc; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)finalMapBuf + i * finalDescSize);
        finalInputs[nFinalInputs].base = d->PhysicalStart;
        finalInputs[nFinalInputs].length = d->NumberOfPages * BOOT_HANDOFF_PAGE_SIZE;
        finalInputs[nFinalInputs].type = memMapEfiTypeToBootMem(d->Type, d->Attribute);
        nFinalInputs++;
    }

    BootMemRegion *outRegions = (BootMemRegion *)(uintptr_t)memMapArrayPhys;
    uint32_t nOutRegions = 0;
    mmst = bootHandoffFinalMap(finalInputs, nFinalInputs, &allocs, finalInputs, HANDOFF_MAX_INPUTS,
                               finalScratch, outRegions, BOOT_HANDOFF_MEMMAP_CAP, &nOutRegions);
    if (mmst != BOOT_OK) {
        /* Nothing left to log this to but COM1 -- ConOut and every other Boot Service are gone. */
        handoffHalt("loader: final memory map normalize failed\n");
    }
    bi->memMapCount = nOutRegions;

    loaderTrampoline((uint64_t)pt.pml4Phys, bootInfoVa, stackTopVa, entryVa);
    for (;;) { /* unreachable */
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}
