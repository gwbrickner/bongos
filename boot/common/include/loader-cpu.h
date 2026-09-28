/* CPUID/MSR/RDTSC/RDSEED/RDRAND primitives shared by both loaders (D-104), moved out of
 * boot/uefi/handoff.c so BIOS stage2 (M2.5) reuses the exact same CPU checks and random-seed
 * gathering rather than a second hand-copied implementation. Genuinely hardware-touching (raw
 * `cpuid`/`rdmsr`/`rdtsc`/`rdseed`/`rdrand`), so this lives in boot/common/hw/ rather than the
 * host-testable boot/common glob (files directly under boot/common, matched by name). */
#ifndef BOOT_COMMON_LOADER_CPU_H
#define BOOT_COMMON_LOADER_CPU_H

#include <stdbool.h>
#include <stdint.h>

/* CPUID 0x80000001 EDX bit 20 (NX): required, ARCHITECTURE §1.3. Returns whether NX is present;
 * `*outHas1G` is set from bit 26 (PDPE1GB, gates 1 GiB HHDM pages -- optional). */
bool loaderCpuCheckLongModeFeatures(bool *outHas1G);

/* CR4.LA57 (bit 12): true if the firmware left 5-level paging enabled. This loader only builds
 * 4-level page tables (ARCHITECTURE §6.3); the caller must refuse to boot rather than let the
 * final `mov cr3` misinterpret the PML4 as a PML5 with zero diagnostic output. */
bool loaderCpuLa57Enabled(void);

/* Sets EFER.NXE (bit 11) via rdmsr/wrmsr on MSR 0xC0000080 -- must happen before CR0.PG is set
 * (the first page-table walk that could see an NX bit). */
void loaderCpuSetEferNxe(void);

uint64_t loaderCpuRdtsc(void);

bool loaderCpuHasRdseed(void); /* CPUID.(EAX=7,ECX=0):EBX bit 18, gated on the max basic leaf */
bool loaderCpuHasRdrand(void); /* CPUID.1:ECX bit 30 */

/* Fills `seed` (8 qwords = 64 bytes) from RDSEED if available (10 retries per 32-bit half),
 * falling back to RDRAND (10 retries) if not, else 0 -- using only the 32-bit register forms
 * (two draws per qword) so the exact same code builds and runs correctly whether this loader
 * target is 32-bit (BIOS stage2) or 64-bit (UEFI). Then XORs a perturbed RDTSC into every qword
 * regardless (ARCHITECTURE §5.5 step 7/§5.6). Does not consult a firmware-specific RNG source
 * (EFI_RNG_PROTOCOL, a thunked BIOS call) -- the caller tries those first and only falls back to
 * this. */
void loaderCpuRandomFill(uint64_t seed[8]);

/* IA32_PAT (MSR 0x277) entry 2: true iff it's still UC- (0x07) or UC (0x00), the documented
 * power-on value (SDM Vol 3A "PAT Compatibility with Earlier IA-32 Processors") that
 * PT_FLAGS_FRAMEBUFFER's 4K leaves (PCD=1/PWT=0) select. The D-068 framebuffer mapping is only
 * actually uncacheable if this holds -- read back rather than assumed, since firmware that
 * reprogrammed entry 2 to something cacheable (e.g. WB) would make the mapping lie about being
 * safe to treat as MMIO. */
bool loaderCpuPatEntry2Uncacheable(void);

#endif
