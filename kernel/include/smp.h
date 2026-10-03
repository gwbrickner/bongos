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
#define SMP_STAGE_TSC     4u /* an AP is waiting for the BSP to run the cross-CPU TSC check (D-198) */
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

/* The IPI vectors (ARCHITECTURE §7.2). */
#define SMP_VECTOR_TLB  0xF0u /* TLB shootdown */
#define SMP_VECTOR_KICK 0xF1u /* reschedule / wake an idle CPU: an empty handler */
#define SMP_VECTOR_CALL 0xF2u /* call-function */
#define SMP_VECTOR_STOP 0xF3u /* stop / panic */

typedef void (*SmpFn)(void *arg);

/* Brings up every AP the MADT lists and `cpus=` allows (INIT-SIPI-SIPI, a trampoline page below
 * 1 MiB, then each AP's own init and idle loop). A failure is logged and boot continues with the
 * CPUs that came up; with no usable trampoline page or no MADT the system stays on the BSP.
 * Boot-time, BSP only, once; IF=1, after timeInit(), vmallocInit() and acpiInit(). */
void smpInit(void);

/* Number of online CPUs (the BSP included) and the mask of their dense ids. No locks; IRQ-safe. */
uint32_t smpOnlineCount(void);
uint64_t smpOnlineMask(void);

/* Runs `fn(arg)` on every online CPU in `cpuMask` (bit = dense id; offline bits are ignored),
 * including the caller's own if its bit is set, and returns once every one has finished (D-195).
 * `fn` runs in IRQ context on the target (IF=0, irqDepth()==1): it must follow the handler rules --
 * no sleeping, no vmalloc/vmm map/unmap, no smpCallFunction() of its own -- and may use the pmm and
 * kmalloc (D-200). The caller's own copy runs inline with IRQs disabled. With only the caller
 * online it runs locally and there are no context restrictions; otherwise (some other CPU in the
 * mask) the caller must have IF=1, irqDepth()==0 and hold no irqsave lock, or this panics
 * (panicBug) rather than risking the A-waits-for-B/B-waits-for-A deadlock. IF stays 1 while it
 * waits, so two CPUs calling each other at once both make progress. A CPU that does not answer in
 * 10 s panics. Not IRQ-safe (waits); may not sleep (nothing sleeps yet). */
void smpCallFunction(uint64_t cpuMask, SmpFn fn, void *arg);

/* smpCallFunction() delivered on `vector` (SMP_VECTOR_TLB or SMP_VECTOR_CALL), so the TLB shootdown
 * and the call-function users are told apart in the per-CPU counters. */
void smpCallFunctionVec(uint64_t cpuMask, SmpFn fn, void *arg, uint32_t vector);

/* Sends the kick IPI to CPU `cpuId` (a no-op handler: it just wakes the CPU from `hlt`). IRQ-safe.
 */
void smpKick(uint32_t cpuId);

/* Panic path (D-197): stops every other CPU (0xF3, then an NMI for those that did not answer in
 * 100 ms) and returns. Takes no locks, never allocates, uses no klog. Safe with IF=0 and from any
 * context; a no-op with one CPU online. */
void smpStopOthers(void);

/* Parks the calling CPU forever, silently (IF=0, `hlt`): what a CPU that lost the race to panic
 * does. Never returns; no locks. */
_Noreturn void smpParkSelf(void);

/* True if CPU `cpuId` has been stopped by smpStopOthers(). No locks; IRQ-safe. */
bool smpCpuStopped(uint32_t cpuId);

/* ktest-only (D-197): while set, the stop IPI marks the CPU stopped and returns instead of parking
 * it, so the stop path can be tested without ending the run; clearing it also clears every
 * `stopped` flag. Panics outside a ktest. */
void smpStopTestMode(bool on);

/* Work for an idle AP (D-202): until M4 has threads, an AP runs posted work from its idle loop in
 * "thread" context: IF=1, irqDepth()==0, on its own kernel stack. `fn` may allocate, take locks and
 * call smpCallFunction(); it must not use KTEST_ASSERT (the ktest state is the BSP's), so tests
 * return results through shared memory and the BSP asserts. */
typedef struct SmpWork {
    SmpFn fn;
    void *arg;
    uint32_t remaining; /* CPUs still to finish; the poster waits for 0 */
} SmpWork;

/* Posts `w` (fn, arg and `remaining` set by the caller; `w` must stay valid until it reaches 0) to
 * AP `cpuId` and kicks it. One work item per CPU at a time: posting while that CPU still has an
 * earlier one pending is a panicBug, and so is posting to the calling CPU or an offline one. */
void smpWorkPost(uint32_t cpuId, SmpWork *w);

/* Waits (IF must be 1) until `w->remaining` reaches 0; panics after 30 s. */
void smpWorkWait(SmpWork *w);

/* Runs `fn(arg)` once on every online CPU in `cpuMask`: the caller's own inline (thread context),
 * every other one in its idle loop, and returns when all are done. Not from a handler. */
void smpWorkRun(uint64_t cpuMask, SmpFn fn, void *arg);

/* Runs the posted work of the calling AP, if any (the idle loop calls it with IF=0; it enables
 * interrupts around `fn`). Returns true if work ran. */
bool smpWorkRunPending(void);

/* The stop NMI of the test mode: true (the CPU is marked stopped) when smpStopTestMode() is on and
 * the NMI is the stop IPI's fallback. Called by the NMI path. */
bool smpStopNmiHook(void);

/* The handler registered on all four IPI vectors by smpInit() (IrqHandler signature). */
void smpIpiHandler(uint32_t vector, void *ctx);

/* The arch hooks (smp-x86.c): send vector `vector` (fixed, edge) or an NMI to CPU `cpuId`.
 * IRQ-safe; `cpuId` must be an online CPU. */
void archSmpSendIpi(uint32_t cpuId, uint32_t vector);
void archSmpSendNmi(uint32_t cpuId);

/* Adds dense id `cpuId` to the online mask (seq-cst). Called by a CPU after it published its
 * cpuTable entry; nothing removes a CPU. IRQ-safe. */
void smpMarkOnline(uint32_t cpuId);

#endif
