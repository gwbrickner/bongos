/* See kernel/include/arch/cpu-init.h. GDT + TSS + IDT (ARCHITECTURE §7.1/§7.2, D-072/D-074): one
 * static BSP-only table built and loaded at runtime -- a TSS base address can't be expressed in a
 * static initializer, and building at runtime is KASLR-safe. LIDT runs *after* LTR: an IST gate
 * taken before TR is valid reads IST from garbage and triple-faults. */
#include <arch/cpu-init.h>

#include "include/cpu-impl.h"
#include "include/gdt.h"
#include "include/trap-impl.h"

#include "panic.h"
#include "sections.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SDM Vol 3A "64-Bit TSS Format". Packed, since the CPU reads these exact byte offsets;
 * `-Waddress-of-packed-member` means every field is read/written by value, never by `&field`. */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7]; /* ist[0] is IST1, ... ist[6] is IST7; only IST1-3 are used (D-073) */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomapBase;
} Tss;
_Static_assert(sizeof(Tss) == 104, "Tss must be the SDM's 104-byte 64-bit TSS layout");
_Static_assert(offsetof(Tss, rsp0) == 0x04, "Tss.rsp0 offset");
_Static_assert(offsetof(Tss, ist) == 0x24, "Tss.ist offset");
_Static_assert(offsetof(Tss, iomapBase) == 0x66, "Tss.iomapBase offset");

typedef struct {
    uint64_t gdt[GDT_ENTRY_COUNT];
    Tss tss;
} ArchCpuTables;

/* aligned(128): the TSS must never straddle a page boundary the same way any other structure the
 * CPU reads directly shouldn't; 128 is comfortably more than enough headroom for 104 bytes while
 * staying a cheap, simple alignment (no need to reach for a full 4096-byte alignment here). */
static ArchCpuTables bspTables __attribute__((aligned(128)));
static bool bspInitDone = false;

/* Slots 0-5 are single-qword descriptors (D-072); slots 6-7 are the two halves of the 16-byte TSS
 * descriptor, filled by tssDescriptorBuild() below. */
static void gdtBuild(uint64_t gdt[GDT_ENTRY_COUNT], const Tss *tss) {
    gdt[0] = 0;                     /* null */
    gdt[1] = 0x00AF9B000000FFFFULL; /* kernel CS: P, DPL0, code R/A, L=1 */
    gdt[2] = 0x00CF93000000FFFFULL; /* kernel DS: P, DPL0, data RW/A */
    gdt[3] = 0;                     /* user CS32 placeholder: NOT present (D-072) */
    gdt[4] = 0x00CFF3000000FFFFULL; /* user DS: P, DPL3, data RW/A */
    gdt[5] = 0x00AFFB000000FFFFULL; /* user CS64: P, DPL3, code R/A, L=1 */

    /* TSS descriptor (SDM Vol 3A "TSS Descriptor in 64-bit Mode"): limit = sizeof(Tss)-1 (byte
     * granularity, G=0), base split across the low/high qwords, type 0x9 = available 64-bit TSS,
     * P=1, DPL=0. Bits 96-127 (the high qword's upper half) are reserved and must be 0. */
    uint64_t base = (uint64_t)(uintptr_t)tss;
    uint64_t limit = sizeof(*tss) - 1;
    gdt[6] =
        limit | ((base & 0xFFFFFFULL) << 16) | (0x89ULL << 40) | (((base >> 24) & 0xFFULL) << 56);
    gdt[7] = base >> 32;
}

/* IST1/2/3 map to #DF/NMI/#MC (D-073); IST4-7 are unused (0, meaning "don't switch stacks" for
 * any gate that doesn't reference them). RSP0 stays 0 (poison, ARCHITECTURE §7.1): there are no
 * ring transitions yet, so a stray one #PFs near-null instead of using a stale/garbage stack.
 * iomapBase = sizeof(Tss) (== the TSS limit + 1) means there is no I/O permission bitmap, so all
 * ring-3 port I/O is denied by construction once userspace exists. */
static void tssBuild(Tss *tss, uint64_t ist1Top, uint64_t ist2Top, uint64_t ist3Top) {
    *tss = (Tss){0};
    tss->rsp0 = 0;
    tss->ist[0] = ist1Top;
    tss->ist[1] = ist2Top;
    tss->ist[2] = ist3Top;
    tss->iomapBase = sizeof(*tss);
}

void archCpuInitBsp(void) {
    if (bspInitDone) {
        panic("cpu-init: archCpuInitBsp() called twice");
    }
    bspInitDone = true;

    tssBuild(&bspTables.tss, (uint64_t)(uintptr_t)kernelIst1Top, (uint64_t)(uintptr_t)kernelIst2Top,
             (uint64_t)(uintptr_t)kernelIst3Top);
    gdtBuild(bspTables.gdt, &bspTables.tss);

    X86DescriptorPtr gdtr = {
        .limit = sizeof(bspTables.gdt) - 1,
        .base = (uint64_t)(uintptr_t)bspTables.gdt,
    };
    archLoadGdt(&gdtr);
    archLoadTss(GDT_SEL_TSS);

    trapIdtInit(); /* must come after archLoadTss(): see this file's top comment */
}
