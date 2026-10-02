/* Clock references and TSC calibration (ARCHITECTURE §7.5, D-177/D-178, ROADMAP M3.3): the ACPI PM
 * timer and the optional HPET as free-running counters the TSC is calibrated against, shared by
 * time-x86.c, clockref.c and the arch ktests. Nothing outside kernel/arch/x86_64/ includes this. */
#ifndef KERNEL_ARCH_X86_64_CLOCKREF_H
#define KERNEL_ARCH_X86_64_CLOCKREF_H

#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name; /* "pmtimer" or "hpet" */
    uint64_t hz;
    /* 0xFFFFFF (24-bit PM timer) or 0xFFFFFFFF; the HPET is read 32 bits at a time */
    uint32_t mask;
    /* port I/O (`port`) vs MMIO (`mmio`) */
    bool io;
    uint16_t port;
    volatile uint32_t *mmio;
} ClockRef;

/* Finds, validates and (once) logs the ACPI PM timer from the FADT; fills `*out` and returns true,
 * or logs why not and returns false. Idempotent: later calls return the cached result without
 * logging again. Needs acpiInit() done and, for an MMIO PM timer, vmmInit(). Boot-time, BSP, IF
 * state irrelevant; not from IRQ context (it may map MMIO). May not sleep. */
bool clockRefPm(ClockRef *out);
/* The same for the HPET (IA-PC HPET 1.0a): maps the registers, turns every comparator's interrupt
 * off, sets ENABLE and clears LEG_RT (so the PIT keeps ISA IRQ 0), and requires the counter to
 * tick. 32-bit accesses only. */
bool clockRefHpet(ClockRef *out);

/* The counter's current value, masked to `r->mask`. One port read or one 32-bit MMIO load; no
 * locks; IRQ-safe. */
uint32_t clockRefRead(const ClockRef *r);

/* Measures the TSC frequency against `ref`: 3 windows of ref->hz/20 ticks (50 ms), each bracketed
 * by ordered TSC reads, median of the three; repeated once with 3 fresh windows if the spread
 * exceeds 1000 ppm. `*hz` receives the result, `*spreadPpm` the (final) spread. STATUS_ERR_INVALID
 * if the arithmetic cannot represent a result, or the reference never advances. IRQs are disabled
 * for each window (about 50 ms at a time) and restored. Boot-time, BSP; may not sleep. */
Status tscCalibrate(const ClockRef *ref, uint64_t *hz, uint32_t *spreadPpm);

/* Whether CPUID reported an invariant TSC (set by archClockInit()). No locks; IRQ-safe. */
bool archTscInvariant(void);

#endif
