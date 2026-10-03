/* x86 interrupt-controller internals shared by irq.c, lapic.c, ioapic.c, pic8259.c and the arch
 * ktests (ARCHITECTURE §7.3, D-172). Nothing outside kernel/arch/x86_64/ includes this; the
 * portable interface is kernel/include/irq.h. Register offsets are the xAPIC MMIO offsets (SDM
 * Vol 3A Table 10-1); in x2APIC mode lapic.c maps each to MSR 0x800 + (offset >> 4). */
#ifndef KERNEL_ARCH_X86_64_APIC_H
#define KERNEL_ARCH_X86_64_APIC_H

#include "acpi-tables.h"
#include "uapi/status.h"

#include <stdbool.h>
#include <stdint.h>

#define LAPIC_REG_ID         0x20
#define LAPIC_REG_VER        0x30
#define LAPIC_REG_TPR        0x80
#define LAPIC_REG_EOI        0xB0
#define LAPIC_REG_SVR        0xF0
#define LAPIC_REG_ISR        0x100 /* 8 registers, 0x10 apart */
#define LAPIC_REG_TMR        0x180
#define LAPIC_REG_IRR        0x200
#define LAPIC_REG_ESR        0x280
#define LAPIC_REG_CMCI       0x2F0
#define LAPIC_REG_ICR_LO     0x300
#define LAPIC_REG_ICR_HI     0x310
#define LAPIC_REG_TIMER      0x320
#define LAPIC_REG_THERM      0x330
#define LAPIC_REG_PERF       0x340
#define LAPIC_REG_LINT0      0x350
#define LAPIC_REG_LINT1      0x360
#define LAPIC_REG_ERROR      0x370
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define LAPIC_LVT_MASKED      (1u << 16)
#define LAPIC_LVT_NMI_MODE    (4u << 8)
#define LAPIC_SVR_VALUE       0x1FFu /* APIC software-enabled, spurious vector 0xFF */
#define LAPIC_SPURIOUS_VECTOR 0xFFu
#define LAPIC_TIMER_VECTOR    0xFEu      /* ARCHITECTURE §7.2; owned by timekeeping (D-179) */
#define LAPIC_LVT_TIMER_TSCDL (2u << 17) /* LVT timer mode bits 18:17 = 10: TSC-deadline */
#define MSR_IA32_TSC_DEADLINE 0x6E0u

/* --- 8259 (pic8259.c) ---------------------------------------------------------------------- */

/* Remaps the master to 0x20-0x27 and the slave to 0x28-0x2F, then masks every line (ICW1 clears
 * the IMR, so the remap must come first). Harmless when no 8259 exists. Boot-time, IF=0. */
void pic8259RemapAndMask(void);
/* The interrupt mask register of the master (slave = 0) or slave (slave != 0). */
uint8_t pic8259ReadImr(int slave); /* no locks; IRQ-safe; may not sleep */
/* The in-service register (OCW3) of the master or slave: how a spurious IRQ7/IRQ15 is told from a
 * real one. No locks; IF must be 0 (the OCW3 select and the read are two port accesses). */
uint8_t pic8259ReadIsr(int slave);
/* End-of-interrupt to the master 8259 only. */
void pic8259EoiMaster(void); /* no locks; IRQ-safe; may not sleep */

/* --- local APIC (lapic.c) ------------------------------------------------------------------ */

/* Enables and initializes the calling CPU's local APIC (D-172): x2APIC when CPUID allows it
 * (never downgrading a firmware-enabled one), else xAPIC through a UC MMIO mapping of the
 * IA32_APIC_BASE address; software-enables it (spurious vector 0xFF), masks every LVT, programs
 * the LAPIC-NMI LINT pin the MADT describes for this CPU, clears errors and TPR, and drains stale
 * in-service bits. `madt` may be NULL. Panics if the CPU has no usable local APIC or its MMIO
 * cannot be mapped. Boot-time, IF=0; written to be called per CPU from M3.5 (the xAPIC mapping is
 * made once, by the first call). */
void lapicInit(const AcpiMadtInfo *madt);

/* An AP's local APIC bring-up (D-194): the same register setup as lapicInit() on the calling CPU,
 * following the BSP's mode and mapping, quiet unless something is wrong. The BSP must have run
 * lapicInit() already. Panics if the AP cannot match the BSP's mode. Boot-time, IF=0, on the AP. */
void lapicInitAp(const AcpiMadtInfo *madt);

/* Sends an IPI to the CPU whose APIC ID is `apicId` (physical destination): `icrLo` is the ICR's
 * low dword (delivery mode, vector, level; e.g. 0x4500 INIT, 0x4600|page SIPI, 0x4000|vector
 * fixed). x2APIC: an mfence then one WRMSR; xAPIC: the two-register sequence in an IRQ-disable
 * section with bounded waits on the delivery status. IRQ-safe; never sleeps. */
void lapicSendIpi(uint32_t apicId, uint32_t icrLo);

/* True once lapicInit() chose x2APIC mode. No locks; IRQ-safe; may not sleep. */
bool lapicIsX2apic(void);
/* This CPU's APIC ID (32-bit in x2APIC mode, 8-bit in xAPIC mode). No locks, IRQ-safe. */
uint32_t lapicId(void);
/* Raw register read/write by xAPIC offset (see the list above); `off` must be a valid register
 * offset for the current mode (an invalid x2APIC MSR takes #GP). No locks; IRQ-safe. */
uint32_t lapicRead(uint32_t off);
void lapicWrite(uint32_t off, uint32_t value);
/* Signals end-of-interrupt for the highest-priority in-service vector. No locks; IRQ-safe. */
void lapicEoi(void);
/* Sends a fixed-delivery, edge IPI with vector `v` to the calling CPU only. Waits (bounded) for
 * the xAPIC delivery-status bit; IRQ-safe (it brackets the two-register xAPIC ICR sequence with
 * an IRQ-disable). The vector is taken as soon as IF=1 and TPR allows. */
void lapicSendSelfIpi(uint8_t v);
/* ISR / IRR / TMR bit for vector `v` of this CPU. No locks; IRQ-safe. */
bool lapicIsrBit(uint8_t v);
bool lapicIrrBit(uint8_t v);
bool lapicTmrBit(uint8_t v);
/* The error status register (writes 0 first, as the SDM requires, then reads). */
uint32_t lapicReadEsr(void);

/* Re-programs this CPU's LAPIC timer registers (divide, LVT mode and vector) for the mode
 * archTimerInit() chose, leaving the count stopped; for a CPU whose lapicInit() just masked the
 * timer (M3.5's APs, and the M3.2 re-init ktest). Before archTimerInit() it does nothing. Boot-time
 * or test, IF=0 on entry. */
void lapicTimerCpuSetup(void);
/* The calibrated LAPIC timer rate at divide 16 (0 before archTimerInit()), and whether the timer
 * runs in TSC-deadline mode. No locks; IRQ-safe. */
uint64_t lapicTimerHz(void);
bool lapicTimerDeadlineMode(void);

/* --- IOAPIC (ioapic.c) --------------------------------------------------------------------- */

/* Maps every IOAPIC of `madt`, masks all their pins, and reserves the GSIs of MADT NMI sources.
 * An IOAPIC that cannot be mapped, or whose GSI range overlaps an earlier one, is logged and
 * skipped. `madt` may be NULL (nothing is mapped). Boot-time, IF=0. */
void ioapicInitAll(const AcpiMadtInfo *madt);
/* How many IOAPICs were successfully initialized. ioapicCount/ioapicGsiUsable/ioapicInfo read
 * state that is immutable after ioapicInitAll(): no locks, IRQ-safe, may not sleep. */
uint32_t ioapicCount(void);
/* True iff `gsi` belongs to an initialized IOAPIC and is not a reserved NMI-source GSI. */
bool ioapicGsiUsable(uint32_t gsi);
/* Programs `gsi`'s redirection entry to `rte` (written masked first, then the final value, so the
 * pin never fires half-programmed). STATUS_ERR_NOT_FOUND if no initialized IOAPIC owns `gsi`.
 * IRQ-disable section around the index/data pair; IRQ-safe; may not sleep. */
Status ioapicWriteRte(uint32_t gsi, uint64_t rte);
/* Sets or clears only the mask bit of `gsi`'s entry. Same errors and locking. */
Status ioapicSetMask(uint32_t gsi, bool masked);
/* Reads `gsi`'s 64-bit entry. Same errors and locking. */
Status ioapicReadRte(uint32_t gsi, uint64_t *out);
/* The GSI base and pin count of IOAPIC `index`; false (outputs untouched) if `index` >=
 * ioapicCount(). */
bool ioapicInfo(uint32_t index, uint32_t *gsiBase, uint32_t *pins);

#endif
