/* x86 SMP internals shared by smp-x86.c and its ktests (D-192): the layout of the AP trampoline
 * page's parameter block, which trampoline/ap-trampoline.asm reads and writes at fixed offsets. */
#ifndef KERNEL_ARCH_X86_64_SMP_IMPL_H
#define KERNEL_ARCH_X86_64_SMP_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AP_TRAMP_PAGE_SIZE 4096u
#define AP_TRAMP_GDT_OFF   0xE00u
#define AP_TRAMP_DATA_OFF  0xF00u
#define AP_TRAMP_MAGIC     0x52545041u /* "APTR" little-endian */

/* trampoline `stage` values the AP writes (smp-x86.c reports the last one it reached). */
#define AP_TRAMP_STAGE_NONE 0u
#define AP_TRAMP_STAGE_REAL 1u /* claimed, in real mode */
#define AP_TRAMP_STAGE_PROT 2u /* protected mode */
#define AP_TRAMP_STAGE_LONG 3u /* about to enable paging: the last write to the page */

typedef struct __attribute__((packed)) ApTrampData {
    uint32_t magic;
    uint32_t claimed;
    uint32_t stage;
    uint32_t targetApicId;
    uint32_t useLeafB;
    uint32_t pml4Phys; /* below 4 GiB: the AP loads it in 32-bit mode */
    uint16_t gdtLimit;
    uint32_t gdtBase;
    uint16_t pad0;
    uint32_t pm32Off;
    uint16_t pm32Sel;
    uint16_t pad1;
    uint32_t lm64Off;
    uint16_t lm64Sel;
    uint16_t pad2;
    uint64_t entry64;
    uint64_t stackTop;
    uint64_t cpuLocal;
    uint64_t kernelCr3;
} ApTrampData;

_Static_assert(offsetof(ApTrampData, magic) == 0x00, "ApTrampData.magic");
_Static_assert(offsetof(ApTrampData, claimed) == 0x04, "ApTrampData.claimed");
_Static_assert(offsetof(ApTrampData, stage) == 0x08, "ApTrampData.stage");
_Static_assert(offsetof(ApTrampData, targetApicId) == 0x0C, "ApTrampData.targetApicId");
_Static_assert(offsetof(ApTrampData, useLeafB) == 0x10, "ApTrampData.useLeafB");
_Static_assert(offsetof(ApTrampData, pml4Phys) == 0x14, "ApTrampData.pml4Phys");
_Static_assert(offsetof(ApTrampData, gdtLimit) == 0x18, "ApTrampData.gdtr");
_Static_assert(offsetof(ApTrampData, gdtBase) == 0x1A, "ApTrampData.gdtr.base");
_Static_assert(offsetof(ApTrampData, pm32Off) == 0x20, "ApTrampData.pm32Ptr");
_Static_assert(offsetof(ApTrampData, pm32Sel) == 0x24, "ApTrampData.pm32Ptr.sel");
_Static_assert(offsetof(ApTrampData, lm64Off) == 0x28, "ApTrampData.lm64Ptr");
_Static_assert(offsetof(ApTrampData, lm64Sel) == 0x2C, "ApTrampData.lm64Ptr.sel");
_Static_assert(offsetof(ApTrampData, entry64) == 0x30, "ApTrampData.entry64");
_Static_assert(offsetof(ApTrampData, stackTop) == 0x38, "ApTrampData.stackTop");
_Static_assert(offsetof(ApTrampData, cpuLocal) == 0x40, "ApTrampData.cpuLocal");
_Static_assert(offsetof(ApTrampData, kernelCr3) == 0x48, "ApTrampData.kernelCr3");
_Static_assert(AP_TRAMP_DATA_OFF + sizeof(ApTrampData) <= AP_TRAMP_PAGE_SIZE,
               "the parameter block must fit in the trampoline page");

/* The embedded flat trampoline (ap-blob.asm): exactly AP_TRAMP_PAGE_SIZE bytes. */
extern const uint8_t apTrampolineBlob[];
extern const uint8_t apTrampolineBlobEnd[];

/* kernel/arch/x86_64/ap-entry.asm: the AP's 64-bit entry. Not a callable C function; its address
 * is what the BSP stores in ApTrampData.entry64. */
void apEntry64(void);

/* ktest-only (D-198): runs the cross-CPU TSC check against AP `cpu` with `skew` ticks added to its
 * readings, to prove the estimator recovers a known offset. Nothing is applied. `*warpsBefore` and
 * `*warpsAfter` are the warps seen before and after the estimate is applied, `*estimate` is the
 * offset found. Returns false if the protocol timed out. Needs IF=1, the AP idle. */
bool archTscSyncTest(uint32_t cpu, int64_t skew, uint32_t *warpsBefore, uint32_t *warpsAfter,
                     int64_t *estimate);

/* ktest-only (D-192): the physical address of the page smpInit() used for the AP trampoline, or 0
 * if it started no AP. The page stays withheld from the allocators (D-080); smpInit() zeroes it
 * after bring-up. */
uint64_t archSmpTrampolinePage(void);

#endif
