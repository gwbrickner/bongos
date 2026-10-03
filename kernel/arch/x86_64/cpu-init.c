/* See kernel/include/arch/cpu-init.h. GDT + TSS + IDT (ARCHITECTURE §7.1/§7.2, D-072/D-074): each
 * CPU's tables live in its CpuLocal (D-190) and are built and loaded at runtime -- a TSS base
 * address can't be expressed in a static initializer, and building at runtime is KASLR-safe. LIDT
 * runs *after* LTR: an IST gate taken before TR is valid reads IST from garbage and triple-faults.
 */
#include <arch/cpu-init.h>

#include "include/cpu-impl.h"
#include "include/gdt.h"
#include "include/trap-impl.h"

#include "cpu-local.h"
#include "panic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool bspInitDone = false;

/* Slots 0-5 are single-qword descriptors (D-072); slots 6-7 are the two halves of the 16-byte TSS
 * descriptor, filled by tssDescriptorBuild() below. */
static void gdtBuild(uint64_t gdt[GDT_ENTRY_COUNT], const X86Tss *tss) {
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
static void tssBuild(X86Tss *tss, uint64_t ist1Top, uint64_t ist2Top, uint64_t ist3Top) {
    *tss = (X86Tss){0};
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

    cpuLocalInitBsp();
    ArchCpuLocal *arch = &cpuLocalBsp.arch;
    tssBuild(&arch->tss, arch->istTop[0], arch->istTop[1], arch->istTop[2]);
    gdtBuild(arch->gdt, &arch->tss);

    X86DescriptorPtr gdtr = {
        .limit = sizeof(arch->gdt) - 1,
        .base = (uint64_t)(uintptr_t)arch->gdt,
    };
    archLoadGdt(&gdtr);
    archLoadTss(GDT_SEL_TSS);

    trapIdtInit(); /* must come after archLoadTss(): see this file's top comment */
}

void archCpuInitAp(CpuLocal *cl) {
    ArchCpuLocal *arch = &cl->arch;
    tssBuild(&arch->tss, arch->istTop[0], arch->istTop[1], arch->istTop[2]);
    gdtBuild(arch->gdt, &arch->tss);

    X86DescriptorPtr gdtr = {
        .limit = sizeof(arch->gdt) - 1,
        .base = (uint64_t)(uintptr_t)arch->gdt,
    };
    archLoadGdt(&gdtr);
    archLoadTss(GDT_SEL_TSS);

    trapIdtLoad(); /* after LTR, like the BSP's */
}
