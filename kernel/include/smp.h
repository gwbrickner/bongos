/* SMP bring-up and cross-CPU services (ARCHITECTURE §7.4, D-190..D-203, ROADMAP M3.5). The pure
 * selection logic is smp-core.c (host-tested); the INIT-SIPI-SIPI sequence, trampoline and AP entry
 * are kernel/arch/x86_64/smp-x86.c. */
#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

#include "acpi-tables.h"
#include "bootinfo.h"
#include "cpu-local.h"

#include <stdbool.h>
#include <stdint.h>

/* CpuLocal.bootStage values. */
#define SMP_STAGE_NONE    0u /* not started */
#define SMP_STAGE_ENTERED 1u /* apMain() is running */
#define SMP_STAGE_ONLINE  2u /* published in cpuTable and the online mask */
#define SMP_STAGE_FAILED  3u /* apMain() gave up (the CPU parks) */

/* --- smp-core.c: pure helpers (no locks, no allocation) ------------------------------------- */

#define SMP_APIC_ID_XAPIC_MAX 254u /* the highest xAPIC ID a SIPI can target (255 is broadcast) */
#define SMP_TRAMP_LIMIT       0x9F000u /* the trampoline page must end at or below this */

/* Parses a "cpus=N" token: N total CPUs including the BSP, 1..cpuMax. Returns the limit to apply
 * (cpuMax when the option is absent). `*invalid` is set (and cpuMax returned) for a value that is
 * not a number or is 0; a value above cpuMax is clamped to cpuMax without setting it. `*present`
 * tells whether the option was there at all. */
uint32_t smpParseCpusOption(const char *cmdline, uint32_t cpuMax, bool *present, bool *invalid);

/* Chooses the APs to start from the MADT's CPU list: every Enabled entry (flags bit 0; an
 * Online-Capable-only entry is not present now, D-170) except the BSP (`bspApicId`), in MADT
 * order, stopping once the total (BSP included) reaches `maxTotal`. When the BSP runs in xAPIC
 * mode an APIC ID above 254 cannot be addressed: it is skipped and counted in `*skippedXapic`.
 * Writes at most `outCap` APIC IDs to `out` and returns how many. */
uint32_t smpSelectAps(const AcpiCpu *cpus, uint32_t cpuCount, uint32_t bspApicId, bool bspX2apic,
                      uint32_t maxTotal, uint32_t *out, uint32_t outCap, uint32_t *skippedXapic);

/* Picks the trampoline page: the highest 4 KiB page that lies wholly inside one USABLE region of
 * `map`, at or above 0x1000 and ending at or below SMP_TRAMP_LIMIT (so its SIPI vector is neither
 * 0 nor in the VGA hole 0xA0-0xBF). false if there is none. */
bool smpPickTrampolinePage(const BootMemRegion *map, uint32_t count, uint64_t *outPhys);

/* --- smp.c / arch ---------------------------------------------------------------------------- */

/* Brings up every AP the MADT lists and `cpus=` allows (INIT-SIPI-SIPI, a trampoline page below
 * 1 MiB, then each AP's own init and idle loop). A failure is logged and boot continues with the
 * CPUs that came up; with no usable trampoline page or no MADT the system stays on the BSP.
 * Boot-time, BSP only, once; IF=1, after timeInit(), vmallocInit() and acpiInit(). */
void smpInit(void);

/* Number of online CPUs (the BSP included) and the mask of their dense ids. No locks; IRQ-safe. */
uint32_t smpOnlineCount(void);
uint64_t smpOnlineMask(void);

/* Adds dense id `cpuId` to the online mask (seq-cst). Called by a CPU after it published its
 * cpuTable entry; nothing removes a CPU. IRQ-safe. */
void smpMarkOnline(uint32_t cpuId);

#endif
