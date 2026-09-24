/* See handoff.h. Implements ARCHITECTURE §5.5 steps 2-10 (D-059/D-060/D-066): CPU checks, reading
 * boot.cfg/kernel.elf, the page-table build (boot/common/paging.c), the BootInfo build, the
 * ExitBootServices retry loop, and the final jump through boot/uefi/trampoline.asm.
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
#include "include/efi/guids.h"
#include "serial.h"

#include "bootcfg.h"
#include "bootmem.h"
#include "elf64.h"
#include "memmap.h"
#include "paging.h"

#include <stdbool.h>

/* boot/uefi/libc-shim.c defines this (D-065: this freestanding target has no libc header to
 * declare it), matching this exact signature. */
extern int memcmp(const void *a, const void *b, size_t n);

#define HANDOFF_PT_POOL_PAGES    1024u /* 4 MiB: generous fixed bound, see file header comment */
#define HANDOFF_BOOT_STACK_PAGES 16u   /* 64 KiB, ARCHITECTURE §5.4 */
#define HANDOFF_MAX_ALLOCS       16u
#define HANDOFF_MAX_INPUTS       512u
#define HANDOFF_MEMMAP_CAP       4096u
#define HANDOFF_MEMMAP_CAP_PAGES 24u /* ceil(4096 * sizeof(BootMemRegion) / 4096) */
#define HANDOFF_PAGE_SIZE        4096ULL

typedef struct {
    uint64_t base;
    uint64_t pages;
    uint32_t type; /* a BootMemType value */
} LoaderAlloc;

static void handoffRecordAlloc(LoaderAlloc *allocs, uint32_t *count, uint64_t base, uint64_t pages,
                               uint32_t type) {
    if (*count < HANDOFF_MAX_ALLOCS) {
        allocs[*count].base = base;
        allocs[*count].pages = pages;
        allocs[*count].type = type;
        (*count)++;
    }
}

/* CPUID 0x80000001 EDX bit 20 (NX, required) and bit 26 (PDPE1GB, gates 1 GiB HHDM pages). */
static bool handoffCpuCheck(bool *outHas1G) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    (void)eax;
    (void)ebx;
    (void)ecx;
    *outHas1G = (edx & (1u << 26)) != 0;
    return (edx & (1u << 20)) != 0;
}

static void handoffSetEferNxe(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11);
    __asm__ volatile("wrmsr" : : "c"(0xC0000080u), "a"(lo), "d"(hi));
}

static uint64_t handoffRdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool handoffRdseed64(uint64_t *out) {
    uint8_t ok;
    __asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(*out), "=qm"(ok));
    return ok != 0;
}

static bool handoffRdrand64(uint64_t *out) {
    uint8_t ok;
    __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(*out), "=qm"(ok));
    return ok != 0;
}

/* EFI_RNG_PROTOCOL if present, else RDSEED (10 retries), else RDRAND (10 retries), else 0; then
 * RDTSC (perturbed per qword) is XORed into every qword regardless, per ARCHITECTURE §5.5 step 7.
 */
static void handoffGatherRandomSeed(EFI_SYSTEM_TABLE *st, uint64_t seed[8]) {
    bool gotRng = false;
    EFI_RNG_PROTOCOL *rng = NULL;
    if (!EFI_ERROR(st->BootServices->LocateProtocol((EFI_GUID *)&gEfiRngProtocolGuid, NULL,
                                                    (VOID **)&rng))) {
        if (!EFI_ERROR(rng->GetRNG(rng, NULL, 64, (UINT8 *)seed))) {
            gotRng = true;
        }
    }
    if (!gotRng) {
        for (int i = 0; i < 8; i++) {
            uint64_t v = 0;
            bool ok = false;
            for (int retry = 0; retry < 10 && !ok; retry++) {
                ok = handoffRdseed64(&v);
            }
            if (!ok) {
                for (int retry = 0; retry < 10 && !ok; retry++) {
                    ok = handoffRdrand64(&v);
                }
            }
            seed[i] = ok ? v : 0;
        }
    }
    uint64_t tsc = handoffRdtsc();
    for (int i = 0; i < 8; i++) {
        seed[i] ^= tsc;
        tsc = tsc * 6364136223846793005ULL + 1; /* cheap avalanche between qwords */
    }
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
    if (!handoffCpuCheck(&has1G)) {
        loaderSerialWriteString("loader: CPU lacks NX; refusing to boot\n");
        return EFI_UNSUPPORTED;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS status = loaderOpenVolume(st, imageHandle, &root);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: failed to open the ESP volume\n");
        return status;
    }

    BootCfg cfg;
    uint8_t *cfgBuf = NULL;
    uint64_t cfgSize = 0;
    status = loaderReadFile(st, root, (CHAR16 *)L"\\bong\\boot.cfg", &cfgBuf, &cfgSize);
    if (!EFI_ERROR(status)) {
        BootStatus bst = bootCfgParse((const char *)cfgBuf, cfgSize, &cfg);
        bs->FreePool(cfgBuf);
        if (bst != BOOT_OK) {
            loaderSerialWriteString("loader: boot.cfg: ");
            loaderSerialWriteString(bootStatusString(bst));
            loaderSerialWriteString("\n");
            return EFI_INVALID_PARAMETER;
        }
    } else {
        loaderSerialWriteString("loader: boot.cfg not found; using defaults\n");
        cfg.hasKernel = false;
        cfg.hasCmdline = false;
        cfg.kernel[0] = '\0';
        cfg.cmdline[0] = '\0';
    }
    if (!cfg.hasKernel) {
        static const char defaultKernel[] = "/bong/kernel.elf";
        bootMemcpy(cfg.kernel, defaultKernel, sizeof(defaultKernel));
    }

    CHAR16 kernelPathW[BOOT_CFG_KERNEL_PATH_MAX];
    loaderAsciiPathToWide(cfg.kernel, kernelPathW, BOOT_CFG_KERNEL_PATH_MAX);

    uint8_t *kernelBuf = NULL;
    uint64_t kernelSize = 0;
    status = loaderReadFile(st, root, kernelPathW, &kernelBuf, &kernelSize);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: failed to read the kernel image\n");
        return status;
    }

    ElfImage elfImage;
    BootStatus bst = elfParse(kernelBuf, kernelSize, &elfImage);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: kernel.elf: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        return EFI_LOAD_ERROR;
    }

    LoaderAlloc allocs[HANDOFF_MAX_ALLOCS];
    uint32_t allocCount = 0;

    EFI_PHYSICAL_ADDRESS kernelPhys = 0;
    UINTN kernelPages = (UINTN)(elfImage.span / HANDOFF_PAGE_SIZE);
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
    handoffRecordAlloc(allocs, &allocCount, (uint64_t)kernelPhys, kernelPages, BOOT_MEM_KERNEL);

    EFI_PHYSICAL_ADDRESS stackPhys = 0;
    status =
        bs->AllocatePages(AllocateAnyPages, EfiLoaderData, HANDOFF_BOOT_STACK_PAGES, &stackPhys);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the boot stack\n");
        return status;
    }
    handoffRecordAlloc(allocs, &allocCount, (uint64_t)stackPhys, HANDOFF_BOOT_STACK_PAGES,
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
    for (UINTN i = 0; i < nPreDesc && nRunsInput < HANDOFF_MAX_INPUTS; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)preMapBuf + i * preDescSize);
        if (handoffIsHhdmType(d->Type)) {
            preRunsInput[nRunsInput].base = d->PhysicalStart;
            preRunsInput[nRunsInput].length = d->NumberOfPages * HANDOFF_PAGE_SIZE;
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
    UINTN handoffPages = 2 + HANDOFF_MEMMAP_CAP_PAGES;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, handoffPages, &handoffPhys);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the handoff block\n");
        return status;
    }
    handoffRecordAlloc(allocs, &allocCount, (uint64_t)handoffPhys, handoffPages,
                       BOOT_MEM_LOADER_RECLAIM);
    uint64_t bootInfoPhys = (uint64_t)handoffPhys;
    uint64_t cmdlinePhys = bootInfoPhys + HANDOFF_PAGE_SIZE;
    uint64_t memMapArrayPhys = cmdlinePhys + HANDOFF_PAGE_SIZE;

    EFI_PHYSICAL_ADDRESS poolPhys = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, HANDOFF_PT_POOL_PAGES, &poolPhys);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: out of memory allocating the page-table pool\n");
        return status;
    }
    handoffRecordAlloc(allocs, &allocCount, (uint64_t)poolPhys, HANDOFF_PT_POOL_PAGES,
                       BOOT_MEM_LOADER_RECLAIM);

    uint64_t trampPhys = bootAlignDown((uint64_t)(uintptr_t)&loaderTrampoline, HANDOFF_PAGE_SIZE);
    handoffRecordAlloc(allocs, &allocCount, trampPhys, 1, BOOT_MEM_LOADER_RECLAIM);

    PtBuilder pt;
    bst = ptInit(&pt, (uint64_t)poolPhys, HANDOFF_PT_POOL_PAGES, has1G);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: page-table init failed\n");
        return EFI_OUT_OF_RESOURCES;
    }
    for (uint32_t i = 0; i < nHhdmRuns; i++) {
        uint64_t base = hhdmRuns[i].base;
        uint64_t length = hhdmRuns[i].length;
        if (base >= BOOTINFO_HHDM_SIZE) {
            continue; /* clipped: beyond the 64 TiB HHDM window */
        }
        if (length > BOOTINFO_HHDM_SIZE - base) {
            length = BOOTINFO_HHDM_SIZE - base;
        }
        bst = ptMapRange(&pt, BOOTINFO_HHDM_BASE + base, base, length, PT_FLAGS_HHDM, true);
        if (bst != BOOT_OK) {
            loaderSerialWriteString("loader: HHDM mapping failed: ");
            loaderSerialWriteString(bootStatusString(bst));
            loaderSerialWriteString("\n");
            return EFI_OUT_OF_RESOURCES;
        }
    }
    bst = ptMapElfImage(&pt, &elfImage, (uint64_t)kernelPhys, 0);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: kernel mapping failed: ");
        loaderSerialWriteString(bootStatusString(bst));
        loaderSerialWriteString("\n");
        return EFI_OUT_OF_RESOURCES;
    }
    bst = ptMapRange(&pt, trampPhys, trampPhys, HANDOFF_PAGE_SIZE, PT_FLAGS_TRAMPOLINE, false);
    if (bst != BOOT_OK) {
        loaderSerialWriteString("loader: trampoline mapping failed\n");
        return EFI_OUT_OF_RESOURCES;
    }

    /* Self-check (ARCHITECTURE §5.5 step 10): every mapping the trampoline and the kernel's first
     * instructions depend on actually resolves the way it should. */
    uint64_t entryVa = elfImage.entry;
    uint64_t stackTopVa = BOOTINFO_HHDM_BASE + (uint64_t)stackPhys +
                          (uint64_t)HANDOFF_BOOT_STACK_PAGES * HANDOFF_PAGE_SIZE;
    uint64_t bootInfoVa = BOOTINFO_HHDM_BASE + bootInfoPhys;
    uint64_t checkPa, checkFlags;
    bool selfCheckOk = ptLookup(&pt, entryVa, &checkPa, &checkFlags) == BOOT_OK &&
                       (checkFlags & PT_W) == 0 && (checkFlags & PT_NX) == 0 &&
                       ptLookup(&pt, stackTopVa - 8, &checkPa, &checkFlags) == BOOT_OK &&
                       (checkFlags & PT_W) != 0 && (checkFlags & PT_NX) != 0 &&
                       ptLookup(&pt, bootInfoVa, &checkPa, &checkFlags) == BOOT_OK &&
                       ptLookup(&pt, trampPhys, &checkPa, &checkFlags) == BOOT_OK &&
                       (checkFlags & PT_NX) == 0;
    if (!selfCheckOk) {
        loaderSerialWriteString("loader: page-table self-check failed\n");
        return EFI_DEVICE_ERROR;
    }

    BootInfo *bi = (BootInfo *)(uintptr_t)bootInfoPhys;
    bootMemset(bi, 0, sizeof(*bi));
    bi->magic = BOOTINFO_MAGIC;
    bi->version = BOOTINFO_VERSION;
    bi->size = sizeof(BootInfo);
    bi->bootMethod = BOOT_METHOD_UEFI;
    bi->memMapPhys = memMapArrayPhys;
    bi->rsdpPhys = rsdpPhys;
    bi->kernelPhysBase = (uint64_t)kernelPhys;
    bi->kernelVirtBase = elfImage.linkBase;
    bi->kernelSize = elfImage.span;
    bi->kaslrSlide = 0;
    bi->cmdlinePhys = cmdlinePhys;
    bi->hhdmBase = BOOTINFO_HHDM_BASE;
    bi->loaderTsc = loaderTsc;
    bi->efiSystemTablePhys = (uint64_t)(uintptr_t)st;
    bootMemcpy(bi->randomSeed, randomSeed, sizeof(bi->randomSeed));

    char *cmdlineDst = (char *)(uintptr_t)cmdlinePhys;
    uint32_t cmdLen = 0;
    bool cmdlineTruncated = false;
    for (; cfg.cmdline[cmdLen] != '\0'; cmdLen++) {
        if (cmdLen >= BOOTINFO_CMDLINE_MAX - 1) {
            cmdlineTruncated = true;
            break;
        }
        cmdlineDst[cmdLen] = cfg.cmdline[cmdLen];
    }
    cmdlineDst[cmdLen] = '\0';
    if (cmdlineTruncated) {
        loaderSerialWriteString("loader: cmdline truncated to fit BOOTINFO_CMDLINE_MAX\n");
    }

    loaderSerialWriteString("loader: exiting boot services\n");
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)L"loader: exiting boot services\r\n");

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
    handoffSetEferNxe();

    uint32_t nFinalInputs = 0;
    for (UINTN i = 0; i < nFinalDesc && nFinalInputs < HANDOFF_MAX_INPUTS; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)finalMapBuf + i * finalDescSize);
        finalInputs[nFinalInputs].base = d->PhysicalStart;
        finalInputs[nFinalInputs].length = d->NumberOfPages * HANDOFF_PAGE_SIZE;
        finalInputs[nFinalInputs].type = memMapEfiTypeToBootMem(d->Type, d->Attribute);
        nFinalInputs++;
    }
    for (uint32_t i = 0; i < allocCount && nFinalInputs < HANDOFF_MAX_INPUTS; i++) {
        finalInputs[nFinalInputs].base = allocs[i].base;
        finalInputs[nFinalInputs].length = allocs[i].pages * HANDOFF_PAGE_SIZE;
        finalInputs[nFinalInputs].type = allocs[i].type;
        nFinalInputs++;
    }

    BootMemRegion *outRegions = (BootMemRegion *)(uintptr_t)memMapArrayPhys;
    uint32_t nOutRegions = 0;
    mmst = memMapNormalize(finalInputs, nFinalInputs, outRegions, HANDOFF_MEMMAP_CAP, &nOutRegions,
                           finalScratch);
    if (mmst != BOOT_OK) {
        /* Nothing left to log this to but COM1 -- ConOut and every other Boot Service are gone. */
        loaderSerialWriteString("loader: final memory map normalize failed\n");
        for (;;) {
            __asm__ volatile("cli");
            __asm__ volatile("hlt");
        }
    }
    bi->memMapCount = nOutRegions;

    loaderTrampoline((uint64_t)pt.pml4Phys, bootInfoVa, stackTopVa, entryVa);
    for (;;) { /* unreachable */
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}
