/* See handoff.h. The BIOS counterpart of boot/uefi/handoff.c (D-107/D-108): gathers BIOS-specific
 * inputs (E820, the thunked disk/GPT/FAT32, VBE, an EBDA/0xE0000 RSDP scan, RDSEED/RDRAND) and
 * calls into the same shared page-table/BootInfo-build core (boothandoff.c) UEFI uses. */
#include "handoff.h"

#include "boot-status.h"
#include "bootacpi.h"
#include "bootblk.h"
#include "bootcfg.h"
#include "boothandoff.h"
#include "bootfat.h"
#include "bootgpt.h"
#include "bootheap.h"
#include "bootmem.h"
#include "disk.h"
#include "e820.h"
#include "elf64.h"
#include "fbtext.h"
#include "loader-cpu.h"
#include "loader-kaslr.h"
#include "loader-serial.h"
#include "memmap.h"
#include "menu.h"
#include "paging.h"
#include "rm.h"
#include "vbe.h"

#include <stdbool.h>

#define E820_MAX_REGIONS 64u

extern void bootTrampoline(uint64_t pml4Phys, uint64_t bootInfoVa, uint64_t stackTopVa,
                           uint64_t entryVa);
extern const uint8_t __trampolineStart[];

static _Noreturn void handoffHalt(const char *msg) {
    loaderSerialWriteString(msg);
    for (;;) {
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}

/* EBDA + 0xE0000-0xFFFFF (ACPI "Finding the RSDP on IA-PC Systems", D-107). The BDA word at
 * physical 0x40E holds the EBDA segment; a plain flat read is safe here since stage2 runs unpaged
 * 32-bit PM with DS covering the whole 4 GiB and A20 already on. A small local D-0xx pins the
 * EBDA sanity range (a real EBDA always falls in [0x80000, 0xA0000)) and the 1 KiB scan length. */
static uint64_t handoffFindRsdp(void) {
    uint16_t ebdaSeg = *(const volatile uint16_t *)bootPhysToPtr(0x40Eu);
    uint32_t ebda = (uint32_t)ebdaSeg << 4;
    if (ebda >= 0x80000u && ebda + 1024u <= 0xA0000u) {
        uint64_t found = bootAcpiScanForRsdp((const uint8_t *)bootPhysToPtr(ebda), 1024, ebda);
        if (found != 0) {
            return found;
        }
    }
    return bootAcpiScanForRsdp((const uint8_t *)bootPhysToPtr(0xE0000u), 0x20000u, 0xE0000u);
}

/* No EFI_RNG_PROTOCOL equivalent on BIOS -- always the RDSEED/RDRAND-then-TSC-jitter fallback
 * loaderCpuRandomFill() implements (the same one UEFI's handoffGatherRandomSeed() falls back to
 * when EFI_RNG_PROTOCOL isn't present). */
static void handoffGatherRandomSeed(uint64_t seed[8]) {
    if (!loaderCpuHasRdseed() && !loaderCpuHasRdrand()) {
        loaderSerialWriteString("loader: no RDSEED or RDRAND; seeding from TSC jitter only\n");
    }
    loaderCpuRandomFill(seed);
}

/* The RAM types ARCHITECTURE §5's HHDM coverage rule maps (D-059), mirrored from UEFI's
 * handoffIsHhdmType -- but starting from e820Scan()'s already-BootMemType-typed output (via
 * memMapE820TypeToBootMem) rather than raw EFI/E820 type codes. Reserved/bad/non-volatile ranges
 * must never become part of the HHDM (D-059/§5.4): on real hardware, mapping the BIOS ROM or a
 * PCI hole write-back is a machine-check risk, and it can make bootHandoffMapAll's framebuffer
 * mapping spuriously conflict with a "USABLE" range that was never really usable. */
static bool handoffIsHhdmType(uint32_t bootMemType) {
    switch (bootMemType) {
        case BOOT_MEM_USABLE:
        case BOOT_MEM_ACPI_RECLAIM:
        case BOOT_MEM_ACPI_NVS:
            return true;
        default:
            return false;
    }
}

/* Several KiB apiece: static rather than on the stack, which otherwise competes with this
 * function's own modest 64 KiB PM stack (stage2.ld's `.stack` section, D-101) -- same reasoning
 * as UEFI's handoff.c file-scope statics. */
static MemMapInput e820Regions[E820_MAX_REGIONS];
static BootMemRegion normalized[E820_MAX_REGIONS];
static uint64_t normScratch[E820_MAX_REGIONS * 2];
static BootHeap heap;
static MemMapInput hhdmInput[E820_MAX_REGIONS];
static BootMemRegion hhdmRuns[E820_MAX_REGIONS * 2];
static uint64_t hhdmScratch[E820_MAX_REGIONS * 2];
static MemMapInput finalWork[E820_MAX_REGIONS + BOOT_HANDOFF_MAX_ALLOCS];
static uint64_t finalScratch[2 * (E820_MAX_REGIONS + BOOT_HANDOFF_MAX_ALLOCS)];

_Noreturn void handoffRun(void) {
    uint64_t loaderTsc = loaderCpuRdtsc(); /* captured before anything else, ARCHITECTURE §5.5 */

    bool has1G = false;
    if (!loaderCpuCheckLongModeFeatures(&has1G)) {
        handoffHalt("loader: CPU lacks long mode or NX; refusing to boot\n");
    }

    /* E820 -> the normalized map -> the heap every later allocation in this function comes from
     * (D-107; steps 8's already-proven plumbing, now feeding the real flow instead of a
     * self-test). */
    uint32_t nE820 = e820Scan(e820Regions, E820_MAX_REGIONS);
    if (nE820 >= E820_MAX_REGIONS) {
        loaderSerialWriteString("loader: E820 map may have been truncated at E820_MAX_REGIONS\n");
    }
    uint32_t nNormalized = 0;
    BootStatus bst = memMapNormalize(e820Regions, nE820, normalized, E820_MAX_REGIONS, &nNormalized,
                                     normScratch);
    if (bst != BOOT_OK) {
        handoffHalt("loader: memMapNormalize failed; halting\n");
    }
    bst = bootHeapInit(&heap, normalized, nNormalized);
    if (bst != BOOT_OK) {
        handoffHalt("loader: bootHeapInit failed; halting\n");
    }

    /* Disk -> GPT -> FAT32 (D-105/D-106; step 9's already-proven plumbing). */
    BootBlockDev dev;
    bst = diskProbe(&dev);
    if (bst != BOOT_OK) {
        handoffHalt("loader: diskProbe failed; halting\n");
    }
    uint8_t scratch[512];
    BootGptPart part;
    uint8_t diskGuid[16];
    bst = bootGptFindPartition(&dev, BOOT_GPT_TYPE_GUID_ESP, scratch, &part, diskGuid);
    if (bst != BOOT_OK) {
        handoffHalt("loader: bootGptFindPartition failed; halting\n");
    }
    BootFatVol vol;
    bst = bootFatMount(&vol, &dev, part.startLba, part.endLba - part.startLba + 1, scratch);
    if (bst != BOOT_OK) {
        handoffHalt("loader: bootFatMount failed; halting\n");
    }

    /* boot.cfg: a missing file falls back to defaults (bootCfgParse("", 0, ...) synthesizes the
     * same single-implicit-entry, all-defaults BootCfg a hand-rolled fallback would build,
     * matching boot/uefi/handoff.c's own precedent), but a *found and malformed* file is fatal. */
    BootFatFile cfgFile;
    BootStatus cfgOpenSt = bootFatOpen(&vol, "/bong/boot.cfg", scratch, &cfgFile);
    static const char emptyCfgText[] = "";
    const char *cfgText = emptyCfgText;
    uint64_t cfgTextLen = 0;
    if (cfgOpenSt == BOOT_OK && cfgFile.size > 0) {
        uint32_t cfgPages = (uint32_t)(bootAlignUp(cfgFile.size, BOOT_HANDOFF_PAGE_SIZE) >> 12);
        uint64_t cfgPhys = 0;
        if (bootHeapAllocPages(&heap, cfgPages, &cfgPhys) == BOOT_OK &&
            bootFatRead(&vol, &cfgFile, (uint8_t *)(uintptr_t)cfgPhys, cfgFile.size, scratch) ==
                BOOT_OK) {
            cfgText = (const char *)(uintptr_t)cfgPhys;
            cfgTextLen = cfgFile.size;
        } else {
            loaderSerialWriteString("loader: boot.cfg read failed; using defaults\n");
        }
    } else {
        loaderSerialWriteString("loader: boot.cfg not found; using defaults\n");
    }

    BootCfg cfg;
    bst = bootCfgParse(cfgText, cfgTextLen, &cfg);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: boot.cfg:");
        loaderSerialWriteUint(cfg.errorLine);
        loaderSerialWriteString(": ");
        loaderSerialWriteString(cfg.errorReason != NULL ? cfg.errorReason : bootStatusString(bst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }

    /* VBE with the global resolution first (D-103), show the menu (if any) on that framebuffer,
     * resolve the selected entry, then re-pick VBE only if the entry's resolution differs from
     * the global one. */
    uint32_t globalResWidth =
        (cfg.global.setMask & BOOT_CFG_HAS_RESOLUTION) ? cfg.global.resWidth : 0;
    uint32_t globalResHeight =
        (cfg.global.setMask & BOOT_CFG_HAS_RESOLUTION) ? cfg.global.resHeight : 0;
    BootFramebuffer fb;
    vbeSetMode(globalResWidth, globalResHeight, &fb);

    uint32_t selectedIndex = cfg.defaultIndex;
    if (cfg.timeoutSec > 0) {
        /* ARCHITECTURE §5.2: the menu runs on screen *and* serial whenever there's a timeout to
         * show one for -- never skipped outright just because no framebuffer came up, matching
         * boot/uefi/handoff.c's own precedent (D-071: loaderMenuRun/the shared menu-ui drawing
         * all tolerate a NULL fx). */
        BootFbText fx;
        BootFbText *fxPtr = NULL;
        if (fb.phys != 0) {
            BootStatus fxSt = fbTextInit(&fx, (uint8_t *)(uintptr_t)fb.phys, fb.width, fb.height,
                                         fb.pitch, fb.redShift, fb.redSize, fb.greenShift,
                                         fb.greenSize, fb.blueShift, fb.blueSize);
            if (fxSt == BOOT_OK) {
                fxPtr = &fx;
            } else {
                loaderSerialWriteString(
                    "loader: framebuffer geometry unusable for the menu; continuing serial-only\n");
            }
        } else {
            loaderSerialWriteString("loader: no framebuffer available; menu is serial-only\n");
        }
        selectedIndex = loaderMenuRun(cfgText, cfgTextLen, &cfg, fxPtr);
    }

    BootCfgEntry entry;
    bst = bootCfgResolveEntry(cfgText, cfgTextLen, &cfg, selectedIndex, &entry);
    if (bst != BOOT_OK) {
        handoffHalt("loader: boot.cfg: failed to resolve the selected entry; halting\n");
    }
    if (entry.resWidth != globalResWidth || entry.resHeight != globalResHeight) {
        vbeSetMode(entry.resWidth, entry.resHeight, &fb);
    }

    /* The kernel image. */
    BootFatFile kernelFile;
    bst = bootFatOpen(&vol, entry.kernel, scratch, &kernelFile);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: failed to open the kernel image: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }
    if (kernelFile.size == 0) {
        handoffHalt("loader: kernel image is empty; halting\n");
    }
    uint32_t kernelFilePages =
        (uint32_t)(bootAlignUp(kernelFile.size, BOOT_HANDOFF_PAGE_SIZE) >> 12);
    uint64_t kernelFilePhys = 0;
    bst = bootHeapAllocPages(&heap, kernelFilePages, &kernelFilePhys);
    if (bst != BOOT_OK) {
        handoffHalt("loader: out of memory reading the kernel image; halting\n");
    }
    uint8_t *kernelFileBuf = (uint8_t *)(uintptr_t)kernelFilePhys;
    bst = bootFatRead(&vol, &kernelFile, kernelFileBuf, kernelFile.size, scratch);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: failed to read the kernel image: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }

    ElfImage elfImage;
    bst = elfParse(kernelFileBuf, kernelFile.size, &elfImage);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: kernel.elf: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }

    /* The random seed drives the KASLR slide below, so it is gathered here (right after the ELF
     * is validated) rather than later; the same 64 bytes still go into BootInfo. */
    uint64_t randomSeed[8];
    handoffGatherRandomSeed(randomSeed);

    BootAllocList allocs;
    bootMemset(&allocs, 0, sizeof(allocs));

    uint32_t kernelPages = (uint32_t)(elfImage.span >> 12);
    uint64_t kernelPhys = 0;
    bst = bootHeapAllocPages(&heap, kernelPages, &kernelPhys);
    if (bst != BOOT_OK) {
        handoffHalt("loader: out of memory allocating the kernel image; halting\n");
    }
    bst = elfLoad(&elfImage, kernelFileBuf, (uint8_t *)(uintptr_t)kernelPhys);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: kernel.elf: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }
    /* KASLR (M2.6, D-120/D-121): relocate the loaded image from kernel.elf's --emit-relocs tables
     * (the whole file is still in kernelFileBuf). Any failure falls back to an unslid boot. */
    uint64_t kaslrSlide = 0;
    bst = loaderKaslrApply(&elfImage, kernelFileBuf, kernelFile.size,
                           (uint8_t *)(uintptr_t)kernelPhys, (const uint8_t *)randomSeed,
                           entry.kaslr, &kaslrSlide);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: kernel.elf: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }
    if (bootAllocAdd(&allocs, kernelPhys, kernelPages, BOOT_MEM_KERNEL) != BOOT_OK) {
        handoffHalt("loader: too many allocation overlays; halting\n");
    }

    /* The boot stack (ARCHITECTURE §5.4). */
    uint64_t stackPhys = 0;
    bst = bootHeapAllocPages(&heap, BOOT_HANDOFF_BOOT_STACK_PAGES, &stackPhys);
    if (bst != BOOT_OK) {
        handoffHalt("loader: out of memory allocating the boot stack; halting\n");
    }
    if (bootAllocAdd(&allocs, stackPhys, BOOT_HANDOFF_BOOT_STACK_PAGES, BOOT_MEM_LOADER_RECLAIM) !=
        BOOT_OK) {
        handoffHalt("loader: too many allocation overlays; halting\n");
    }

    uint64_t rsdpPhys = handoffFindRsdp();

    /* The HHDM run set: only USABLE/ACPI_RECLAIM/ACPI_NVS from the E820 map, retyped to USABLE
     * (rank is irrelevant for this pass -- one type going in), then normalized/coalesced. */
    uint32_t nHhdmInput = 0;
    for (uint32_t i = 0; i < nE820; i++) {
        if (handoffIsHhdmType(e820Regions[i].type)) {
            hhdmInput[nHhdmInput].base = e820Regions[i].base;
            hhdmInput[nHhdmInput].length = e820Regions[i].length;
            hhdmInput[nHhdmInput].type = BOOT_MEM_USABLE;
            nHhdmInput++;
        }
    }
    uint32_t nHhdmRuns = 0;
    BootStatus mmst = memMapNormalize(hhdmInput, nHhdmInput, hhdmRuns, E820_MAX_REGIONS * 2,
                                      &nHhdmRuns, hhdmScratch);
    if (mmst != BOOT_OK) {
        loaderSerialWriteString("loader: HHDM run set: ");
        loaderSerialWriteString(bootStatusString(mmst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }

    /* The handoff block (BootInfo + cmdline + the memory-map array) and the page-table pool. */
    uint64_t handoffPhys = 0;
    uint32_t handoffPages = 2 + BOOT_HANDOFF_MEMMAP_CAP_PAGES;
    bst = bootHeapAllocPages(&heap, handoffPages, &handoffPhys);
    if (bst != BOOT_OK) {
        handoffHalt("loader: out of memory allocating the handoff block; halting\n");
    }
    if (bootAllocAdd(&allocs, handoffPhys, handoffPages, BOOT_MEM_LOADER_RECLAIM) != BOOT_OK) {
        handoffHalt("loader: too many allocation overlays; halting\n");
    }
    uint64_t bootInfoPhys = handoffPhys;
    uint64_t cmdlinePhys = bootInfoPhys + BOOT_HANDOFF_PAGE_SIZE;
    uint64_t memMapArrayPhys = cmdlinePhys + BOOT_HANDOFF_PAGE_SIZE;

    uint64_t poolPhys = 0;
    bst = bootHeapAllocPages(&heap, BOOT_HANDOFF_PT_POOL_PAGES, &poolPhys);
    if (bst != BOOT_OK) {
        handoffHalt("loader: out of memory allocating the page-table pool; halting\n");
    }
    if (bootAllocAdd(&allocs, poolPhys, BOOT_HANDOFF_PT_POOL_PAGES, BOOT_MEM_LOADER_RECLAIM) !=
        BOOT_OK) {
        handoffHalt("loader: too many allocation overlays; halting\n");
    }

    uint64_t trampPhys = (uint64_t)(uintptr_t)__trampolineStart;
    if (bootAllocAdd(&allocs, trampPhys, 1, BOOT_MEM_LOADER_RECLAIM) != BOOT_OK) {
        handoffHalt("loader: too many allocation overlays; halting\n");
    }

    PtBuilder pt;
    bst = ptInit(&pt, poolPhys, BOOT_HANDOFF_PT_POOL_PAGES, has1G);
    if (bst != BOOT_OK) {
        handoffHalt("loader: page-table init failed; halting\n");
    }
    if ((pt.pml4Phys >> 32) != 0) {
        /* Can't happen in practice -- bootHeapAllocPages only ever hands out [1 MiB, 4 GiB) -- but
         * bootTrampoline's cdecl call only ever reads pml4Phys's low dword, so this is the one
         * cheap place left to catch a heap-contract violation before it becomes a silent CR3
         * truncation instead of a loud halt. */
        handoffHalt("loader: page-table pool above 4 GiB; halting\n");
    }

    BootPtPlan plan = {
        .hhdmRuns = hhdmRuns,
        .hhdmRunCount = nHhdmRuns,
        .elfImage = &elfImage,
        .kernelPhys = kernelPhys,
        .slide = kaslrSlide,
        .trampPhys = trampPhys,
        .patEntry2Uncacheable = loaderCpuPatEntry2Uncacheable(),
    };
    const char *fbNote = NULL;
    bst = bootHandoffMapAll(&pt, &plan, &fb, &allocs, &fbNote);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: page-table build failed: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        handoffHalt("loader: halting\n");
    }
    if (fbNote != NULL) {
        loaderSerialWriteString("loader: framebuffer unusable for handoff: ");
        loaderSerialWriteString(fbNote);
        loaderSerialWriteString("\n");
    }

    uint64_t entryVa = elfImage.entry + kaslrSlide;
    uint64_t stackTopVa = BOOTINFO_HHDM_BASE + stackPhys +
                          (uint64_t)BOOT_HANDOFF_BOOT_STACK_PAGES * BOOT_HANDOFF_PAGE_SIZE;
    uint64_t bootInfoVa = BOOTINFO_HHDM_BASE + bootInfoPhys;
    uint64_t cmdlineVa = BOOTINFO_HHDM_BASE + cmdlinePhys;
    uint64_t memMapVa = BOOTINFO_HHDM_BASE + memMapArrayPhys;
    if (!bootHandoffSelfCheck(&pt, entryVa, stackTopVa, bootInfoVa, cmdlineVa, memMapVa, trampPhys,
                              &fb)) {
        handoffHalt("loader: page-table self-check failed; halting\n");
    }

    BootInfo *bi = (BootInfo *)(uintptr_t)bootInfoPhys;
    BootHandoffFields fields = {
        .bootMethod = BOOT_METHOD_BIOS,
        .fb = fb,
        .rsdpPhys = rsdpPhys,
        .kernelPhys = kernelPhys,
        .kernelVirtBase = elfImage.linkBase + kaslrSlide,
        .kernelSize = elfImage.span,
        .kaslrSlide = kaslrSlide,
        .cmdlinePhys = cmdlinePhys,
        .hhdmBase = BOOTINFO_HHDM_BASE,
        .loaderTsc = loaderTsc,
        .efiSystemTablePhys = 0,
        .randomSeed = (const uint8_t *)randomSeed,
    };
    bootHandoffFillInfo(bi, &fields);
    bi->memMapPhys = memMapArrayPhys;
    bootMemcpy(bi->bootDiskGuid, diskGuid, 16);
    bootMemcpy(bi->bootPartGuid, part.uniqueGuid, 16);

    /* Wipe the loader's stack-local seed copy now that it's in the BootInfo page (same reasoning
     * as UEFI's handoff.c: it would otherwise linger in memory that becomes plain USABLE once the
     * loader's own allocations are freed, recoverable by later kernel code). volatile so this
     * store survives dead-store elimination on a local about to go out of scope. */
    for (int i = 0; i < 8; i++) {
        *(volatile uint64_t *)&randomSeed[i] = 0;
    }

    char *cmdlineDst = (char *)(uintptr_t)cmdlinePhys;
    uint32_t cmdLen = 0;
    while (entry.cmdline[cmdLen] != '\0') {
        cmdLen++;
    }
    bootMemcpy(cmdlineDst, entry.cmdline, cmdLen + 1);
    if (entry.cmdlineTruncated) {
        loaderSerialWriteString("loader: cmdline truncated to fit BOOTINFO_CMDLINE_MAX\n");
    }

    /* The final memory map: the raw E820 map (already BootMemType-typed by e820Scan) merged with
     * every allocation overlay recorded above -- called last, since bootHandoffMapAll can still
     * add a FRAMEBUFFER overlay to `allocs`. There is no BIOS equivalent of ExitBootServices: the
     * E820 map gathered at the very start of this function already *is* the final map. */
    loaderSerialWriteString("loader: entering long mode\n");
    BootMemRegion *outRegions = (BootMemRegion *)(uintptr_t)memMapArrayPhys;
    uint32_t nOutRegions = 0;
    mmst = bootHandoffFinalMap(e820Regions, nE820, &allocs, finalWork,
                               E820_MAX_REGIONS + BOOT_HANDOFF_MAX_ALLOCS, finalScratch, outRegions,
                               BOOT_HANDOFF_MEMMAP_CAP, &nOutRegions);
    if (mmst != BOOT_OK) {
        handoffHalt("loader: final memory map normalize failed; halting\n");
    }
    bi->memMapCount = nOutRegions;

    bootTrampoline(pt.pml4Phys, bootInfoVa, stackTopVa, entryVa);
    for (;;) { /* unreachable */
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}
