/* The shared page-table/BootInfo-build core (ARCHITECTURE §5.5 steps 9-11 / §5.6 step 8, D-108),
 * factored out of boot/uefi/handoff.c so BIOS stage2 (M2.5) calls the exact same logic instead of
 * a second hand-copied implementation. Everything here is pure (no firmware calls, no port I/O,
 * no logging) so it host-tests the same way as paging.c/memmap.c: each loader's own handoff.c
 * gathers firmware-specific inputs (EFI GetMemoryMap vs. E820, EFI_RNG_PROTOCOL/RDSEED vs.
 * thunked-RDSEED, EFI config tables vs. an EBDA/0xE0000 RSDP scan) and calls into this. */
#ifndef BOOT_COMMON_BOOTHANDOFF_H
#define BOOT_COMMON_BOOTHANDOFF_H

#include <stdbool.h>
#include <stdint.h>

#include "bootinfo.h"
#include "boot-status.h"
#include "elf64.h"
#include "memmap.h"
#include "paging.h"

#define BOOT_HANDOFF_MAX_ALLOCS 16u

/* Shared between both loaders (D-108) so their copies can't drift: the page-table pool size, the
 * boot stack size (ARCHITECTURE §5.4), and the BootInfo memory-map array's capacity (in entries
 * and in the pages that back it). */
#define BOOT_HANDOFF_PT_POOL_PAGES                                                                 \
    1024u                                 /* 4 MiB: generous fixed bound (not computed from the    \
                                              exact formula in the design doc -- safe for          \
                                              anything up to a few hundred memory-map regions,     \
                                              comfortably true under QEMU) */
#define BOOT_HANDOFF_BOOT_STACK_PAGES 16u /* 64 KiB */
#define BOOT_HANDOFF_MEMMAP_CAP       4096u
#define BOOT_HANDOFF_MEMMAP_CAP_PAGES 24u /* ceil(4096 * sizeof(BootMemRegion) / 4096) */
#define BOOT_HANDOFF_PAGE_SIZE        4096ULL

_Static_assert(BOOT_HANDOFF_MEMMAP_CAP_PAGES *BOOT_HANDOFF_PAGE_SIZE >=
                   (uint64_t)BOOT_HANDOFF_MEMMAP_CAP * sizeof(BootMemRegion),
               "BOOT_HANDOFF_MEMMAP_CAP_PAGES is too small for BOOT_HANDOFF_MEMMAP_CAP entries -- "
               "update it (and this assert) together if either constant or BootMemRegion's size "
               "changes");

typedef struct {
    uint64_t base;
    uint64_t pages;
    uint32_t type; /* a BootMemType value */
} BootAlloc;

typedef struct {
    BootAlloc allocs[BOOT_HANDOFF_MAX_ALLOCS];
    uint32_t count;
} BootAllocList;

/* Records one allocation overlay (the loader's convention for "this range is not plain USABLE
 * memory", ARCHITECTURE §5.5 step 10). Returns BOOT_ERR_MEMMAP_CAPACITY once `list->count` would
 * exceed BOOT_HANDOFF_MAX_ALLOCS rather than silently dropping the record -- every current call
 * site is a small, fixed, compile-time-known sequence, so this can never actually trip, but
 * dropping an overlay silently would let an allocation quietly reappear as USABLE memory, exactly
 * the class of bug worth failing loudly over instead. The caller halts on failure (this function
 * itself never logs or halts: pure). No locks, boot-time or host-test only. */
BootStatus bootAllocAdd(BootAllocList *list, uint64_t base, uint64_t pages, uint32_t type);

/* Everything the page-table build needs beyond `pt` itself. `hhdmRuns`/`hhdmRunCount` is the
 * already-normalized HHDM run set (memMapNormalize's output); `patEntry2Uncacheable` is the
 * firmware-specific IA32_PAT-entry-2 check (rdmsr) the caller must do itself, since reading an MSR
 * isn't pure -- passed in as a plain bool so the framebuffer-mapping decision below stays pure. */
typedef struct {
    const BootMemRegion *hhdmRuns;
    uint32_t hhdmRunCount;
    const ElfImage *elfImage;
    uint64_t kernelPhys;
    uint64_t trampPhys;
    bool patEntry2Uncacheable;
} BootPtPlan;

/* Maps the HHDM run set, the kernel ELF image, and the identity trampoline page into `pt`, then
 * attempts the framebuffer mapping (D-068: 4 KiB pages only, PT_FLAGS_FRAMEBUFFER, checked
 * against the HHDM window and against every page it would cover already being unmapped). A
 * framebuffer problem (PAT entry 2 not UC/UC-, bad geometry, beyond the HHDM window, overlapping
 * an existing mapping, or the mapping call itself failing) is never fatal: `*fb` is zeroed (the
 * "not provided" convention, D-064), `*fbNote` is set to a static, human-readable reason (NULL if
 * the framebuffer mapped fine or `fb->phys` was already 0), and no BOOT_MEM_FRAMEBUFFER overlay is
 * recorded. Returns an error only for a real page-table failure in the HHDM/kernel/trampoline
 * mappings (those *are* fatal -- unlike the framebuffer, the kernel cannot run without them).
 * Records BOOT_MEM_FRAMEBUFFER into `allocs` on a successful framebuffer mapping. Pure: the
 * caller logs `*fbNote`/the returned BootStatus itself. No locks, boot-time or host-test only. */
BootStatus bootHandoffMapAll(PtBuilder *pt, const BootPtPlan *plan, BootFramebuffer *fb,
                             BootAllocList *allocs, const char **fbNote);

/* Self-check (ARCHITECTURE §5.5 step 10 / §5.6 step 8): true iff the kernel entry point (R-X,
 * non-writable/executable... i.e. executable and not NX, not writable), the top of the boot stack
 * (RW, NX), and the whole handoff block (BootInfo/cmdline/memory-map pages, all present) resolve
 * as expected, the trampoline page resolves present and not NX, and -- if `fb->phys != 0` -- both
 * the first and last page of the mapped framebuffer range resolve to the right physical address
 * with PT_PCD/PT_NX/PT_W set. Pure; read-only. No locks, boot-time or host-test only. */
bool bootHandoffSelfCheck(const PtBuilder *pt, uint64_t entryVa, uint64_t stackTopVa,
                          uint64_t bootInfoVa, uint64_t cmdlineVa, uint64_t memMapVa,
                          uint64_t trampPhys, const BootFramebuffer *fb);

/* Everything BootInfo's fixed fields (ARCHITECTURE §5.3) need beyond `memMapCount` itself, which
 * the caller fills in separately after normalizing the final (post-ExitBootServices-or-equivalent)
 * memory map -- that has to happen strictly after this, so it isn't part of this struct. */
typedef struct {
    uint32_t bootMethod; /* a BootMethod value */
    BootFramebuffer fb;
    uint64_t rsdpPhys;
    uint64_t kernelPhys, kernelVirtBase, kernelSize;
    uint64_t cmdlinePhys;
    uint64_t hhdmBase;
    uint64_t loaderTsc;
    uint64_t efiSystemTablePhys; /* 0 on BIOS */
    const uint8_t *randomSeed;   /* exactly 64 bytes */
} BootHandoffFields;

/* Zeroes `*bi` and fills every fixed field from `fields` (magic/version/size/bootMethod/fb/
 * rsdpPhys/kernel fields/kaslrSlide=0/cmdlinePhys/hhdmBase/loaderTsc/efiSystemTablePhys/
 * randomSeed).
 * Does not touch `memMapPhys`/`memMapCount` (the caller's memory-map array physical address is
 * known before this runs but its count only after -- the caller sets both itself). Pure. No
 * locks, boot-time or host-test only. */
void bootHandoffFillInfo(BootInfo *bi, const BootHandoffFields *fields);

/* Merges the firmware's final raw memory-map descriptors (`fwInputs`, already converted to
 * MemMapInput/BootMemType by the caller's own EFI-type or E820-type mapping) with the recorded
 * allocation overlays (`allocs`) into one `work` buffer, then normalizes it into `out`
 * (ARCHITECTURE §5.5 step 10/§6.1's HHDM contract) -- this is exactly what both loaders do right
 * after their own equivalent of ExitBootServices, factored out once. Returns
 * BOOT_ERR_MEMMAP_CAPACITY if `nFwInputs + allocs->count` exceeds `workCap`, or whatever
 * memMapNormalize() itself returns. Pure. No locks, boot-time or host-test only. */
BootStatus bootHandoffFinalMap(const MemMapInput *fwInputs, uint32_t nFwInputs,
                               const BootAllocList *allocs, MemMapInput *work, uint32_t workCap,
                               uint64_t *scratch, BootMemRegion *out, uint32_t outCap,
                               uint32_t *nOut);

#endif
