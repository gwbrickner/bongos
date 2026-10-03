/* Local APIC driver (ARCHITECTURE §7.3, D-172). x2APIC (MSR interface) when CPUID allows it,
 * otherwise xAPIC over a UC MMIO mapping. Physical destination mode and Fixed delivery only, so
 * neither DFR nor LDR is ever programmed (DFR does not exist in x2APIC mode and LDR is read-only
 * there). Everything here is per-CPU hardware state: no locks. */
#include "include/apic.h"

#include "include/cpu-impl.h"

#include "klog.h"
#include "panic.h"
#include "vmm.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stdint.h>

#define MSR_IA32_APIC_BASE 0x1Bu
#define APIC_BASE_BSP      (1ULL << 8)
#define APIC_BASE_EXTD     (1ULL << 10)
#define APIC_BASE_EN       (1ULL << 11)
#define APIC_BASE_ADDR     0x000FFFFFFFFFF000ULL

#define MSR_X2APIC_BASE     0x800u
#define MSR_X2APIC_ICR      0x830u
#define MSR_X2APIC_SELF_IPI 0x83Fu

#define ICR_DELIVERY_PENDING (1u << 12)
#define ICR_LEVEL_ASSERT     (1u << 14)
#define ICR_SHORTHAND_SELF   (1u << 18)

static bool x2apic = false;
static volatile uint8_t *mmio = NULL; /* xAPIC register window, mapped by the first lapicInit() */
static bool mapped = false;

bool lapicIsX2apic(void) {
    return x2apic;
}

uint32_t lapicRead(uint32_t off) {
    if (x2apic) {
        return (uint32_t)archRdmsr(MSR_X2APIC_BASE + (off >> 4));
    }
    return *(volatile uint32_t *)(mmio + off);
}

void lapicWrite(uint32_t off, uint32_t value) {
    if (x2apic) {
        archWrmsr(MSR_X2APIC_BASE + (off >> 4), value);
    } else {
        *(volatile uint32_t *)(mmio + off) = value;
    }
}

uint32_t lapicId(void) {
    uint32_t id = lapicRead(LAPIC_REG_ID);
    return x2apic ? id : id >> 24;
}

void lapicEoi(void) {
    lapicWrite(LAPIC_REG_EOI, 0); /* x2APIC: any nonzero value raises #GP */
}

static bool lapicBit(uint32_t base, uint8_t v) {
    return (lapicRead(base + 0x10u * (v / 32u)) >> (v % 32u)) & 1u;
}

bool lapicIsrBit(uint8_t v) {
    return lapicBit(LAPIC_REG_ISR, v);
}

bool lapicIrrBit(uint8_t v) {
    return lapicBit(LAPIC_REG_IRR, v);
}

bool lapicTmrBit(uint8_t v) {
    return lapicBit(LAPIC_REG_TMR, v);
}

uint32_t lapicReadEsr(void) {
    lapicWrite(LAPIC_REG_ESR, 0);
    return lapicRead(LAPIC_REG_ESR);
}

#define ICR_SPIN_LIMIT 100000u

static void icrWaitIdle(void) {
    for (uint32_t i = 0; i < ICR_SPIN_LIMIT; i++) {
        if ((lapicRead(LAPIC_REG_ICR_LO) & ICR_DELIVERY_PENDING) == 0) {
            return;
        }
        archPause();
    }
    klogWrite(KLOG_WARN, "lapic", "ICR delivery status stuck busy");
}

void lapicSendSelfIpi(uint8_t v) {
    if (x2apic) {
        archWrmsr(MSR_X2APIC_SELF_IPI, v); /* no delivery-status bit exists in x2APIC mode */
        return;
    }
    uint64_t f = archIrqSave();
    icrWaitIdle();
    lapicWrite(LAPIC_REG_ICR_HI, 0);
    /* Fixed (000), physical (0), level assert, edge, shorthand Self. The low write sends. */
    lapicWrite(LAPIC_REG_ICR_LO, v | ICR_LEVEL_ASSERT | ICR_SHORTHAND_SELF);
    icrWaitIdle();
    archIrqRestore(f);
}

void lapicSendIpi(uint32_t apicId, uint32_t icrLo) {
    if (x2apic) {
        /* WRMSR to an x2APIC MSR is not serializing: without the fence the stores the receiver is
         * about to read (mailbox slots, the trampoline page) may not be visible yet (SDM Vol 3A
         * §10.12.3). */
        archMfence();
        archWrmsr(MSR_X2APIC_ICR, ((uint64_t)apicId << 32) | icrLo);
        return;
    }
    uint64_t f = archIrqSave();
    icrWaitIdle();
    lapicWrite(LAPIC_REG_ICR_HI, apicId << 24);
    lapicWrite(LAPIC_REG_ICR_LO, icrLo); /* the low write sends */
    icrWaitIdle();
    archIrqRestore(f);
}

static bool vendorIsIntel(void) {
    uint32_t r[4];
    archCpuid(0, 0, r);
    return r[1] == 0x756E6547u && r[3] == 0x49656E69u && r[2] == 0x6C65746Eu; /* GenuineIntel */
}

/* The MADT UID of the CPU whose APIC ID is `id`, or ACPI_LAPIC_NMI_ALL if the MADT does not list
 * it (so only "all processors" NMI entries match). */
static uint32_t madtUidFor(const AcpiMadtInfo *madt, uint32_t id) {
    if (madt != NULL) {
        for (uint32_t i = 0; i < madt->cpuCount; i++) {
            if (madt->cpus[i].apicId == id) {
                return madt->cpus[i].uid;
            }
        }
    }
    return ACPI_LAPIC_NMI_ALL;
}

/* One CPU's local APIC bring-up. `bsp` is the boot CPU: it chooses the mode and maps the xAPIC
 * window, and it alone logs; an AP follows the BSP's mode (x2APIC on every CPU or none) and stays
 * quiet unless something is wrong (D-194). */
static void lapicInitCpu(const AcpiMadtInfo *madt, bool bsp) {
    uint32_t r[4];
    archCpuid(1, 0, r);
    bool x2Supported = (r[2] >> 21) & 1u;

    /* IA32_APIC_BASE exists on every CPU that meets ARCHITECTURE §1.3. While EN=0 CPUID.1:EDX[9]
     * reads 0 (SDM Vol 3A §10.4.3), so the CPUID check comes after making sure EN is set. */
    uint64_t base = archRdmsr(MSR_IA32_APIC_BASE);
    if ((base & APIC_BASE_ADDR) == 0) {
        panic("lapic: IA32_APIC_BASE has no base address (0x%llx)", (unsigned long long)base);
    }
    if ((base & APIC_BASE_EN) == 0) {
        archWrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_EN);
        base = archRdmsr(MSR_IA32_APIC_BASE);
        if ((base & APIC_BASE_EN) == 0) {
            panic("lapic: cannot enable the local APIC (IA32_APIC_BASE=0x%llx)",
                  (unsigned long long)base);
        }
    }
    archCpuid(1, 0, r);
    if (((r[3] >> 9) & 1u) == 0) {
        panic("lapic: no usable local APIC (CPUID.1:EDX[9]=0)");
    }
    if (bsp && (base & APIC_BASE_BSP) == 0) {
        klogWrite(KLOG_WARN, "lapic", "the boot CPU's IA32_APIC_BASE.BSP bit is clear");
    }

    /* SDM Vol 3A §10.12.5: never go from x2APIC back to xAPIC (that needs a pass through the
     * disabled state, which resets the APIC), and never go from disabled straight to x2APIC (#GP):
     * EN is already set here. */
    if (bsp) {
        if ((base & APIC_BASE_EXTD) != 0) {
            x2apic = true;
        } else if (x2Supported) {
            archWrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_EN | APIC_BASE_EXTD);
            base = archRdmsr(MSR_IA32_APIC_BASE);
            x2apic = true;
        }
    } else if (x2apic && (base & APIC_BASE_EXTD) == 0) {
        if (!x2Supported) {
            panic("lapic: an AP lacks x2APIC but the BSP runs it");
        }
        archWrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_EN | APIC_BASE_EXTD);
        base = archRdmsr(MSR_IA32_APIC_BASE);
    } else if (!x2apic && (base & APIC_BASE_EXTD) != 0) {
        panic("lapic: an AP is in x2APIC mode but the BSP is not");
    }
    if (!x2apic && !mapped) {
        volatile void *va;
        Status st = vmmMapMmio(base & APIC_BASE_ADDR, 4096, &va);
        if (st != STATUS_OK) {
            panic("lapic: cannot map the local APIC at 0x%llx (status %d)",
                  (unsigned long long)(base & APIC_BASE_ADDR), (int)st);
        }
        mmio = va;
        mapped = true;
    }
    if (bsp && madt != NULL && madt->lapicAddress != (base & APIC_BASE_ADDR)) {
        klogWrite(
            KLOG_WARN, "lapic", "MADT local APIC address 0x%llx differs from the MSR's 0x%llx",
            (unsigned long long)madt->lapicAddress, (unsigned long long)(base & APIC_BASE_ADDR));
    }

    /* Software-enable first: while software-disabled, the LVT mask bits are forced to 1. */
    lapicWrite(LAPIC_REG_SVR, LAPIC_SVR_VALUE);

    uint32_t ver = lapicRead(LAPIC_REG_VER);
    uint32_t maxLvt = (ver >> 16) & 0xFFu;

    lapicWrite(LAPIC_REG_TIMER, LAPIC_LVT_MASKED);
    lapicWrite(LAPIC_REG_TIMER_INIT, 0); /* stops a one-shot/periodic count the firmware left */
    lapicWrite(LAPIC_REG_LINT0, LAPIC_LVT_MASKED); /* the 8259 virtual wire is not used */
    lapicWrite(LAPIC_REG_LINT1, LAPIC_LVT_MASKED);
    lapicWrite(LAPIC_REG_ERROR, LAPIC_LVT_MASKED);
    if (maxLvt >= 4) {
        lapicWrite(LAPIC_REG_PERF, LAPIC_LVT_MASKED);
    }
    if (maxLvt >= 5) {
        lapicWrite(LAPIC_REG_THERM, LAPIC_LVT_MASKED);
    }
    /* CMCI is Intel-only; on AMD the x2APIC MSR 0x82F does not exist (#GP). AMD's extended LVTs
     * (0x500+) belong to firmware and are left alone. */
    if (maxLvt >= 6 && vendorIsIntel()) {
        lapicWrite(LAPIC_REG_CMCI, LAPIC_LVT_MASKED);
    }

    lapicWrite(LAPIC_REG_ESR, 0); /* the SDM wants two back-to-back writes to clear it */
    lapicWrite(LAPIC_REG_ESR, 0);
    lapicWrite(LAPIC_REG_TPR, 0);

    /* Firmware may have left vectors in service (an EOI never sent); retire them now, before IF=1
     * makes every pending IRR bit deliverable. */
    uint32_t drained = 0;
    for (uint32_t pass = 0; pass < 256; pass++) {
        bool any = false;
        for (uint32_t i = 0; i < 8; i++) {
            any = any || lapicRead(LAPIC_REG_ISR + 0x10u * i) != 0;
        }
        if (!any) {
            break;
        }
        lapicEoi();
        drained++;
    }
    if (drained != 0 && bsp) {
        klogWrite(KLOG_WARN, "lapic", "firmware left %u vectors in service; retired", drained);
    }
    uint32_t pending = 0;
    for (uint32_t i = 0; i < 8; i++) {
        pending += (uint32_t)__builtin_popcount(lapicRead(LAPIC_REG_IRR + 0x10u * i));
    }
    if (pending != 0 && bsp) {
        klogWrite(KLOG_WARN, "lapic",
                  "%u vectors pending at init; they deliver, unhandled, once IF=1", pending);
    }

    uint32_t id = lapicId();
    /* The LAPIC-NMI pin for this CPU (MADT type 4): NMI delivery, edge-triggered always (SDM
     * Vol 3A §10.5.1), polarity from the INTI flags. Without a match the pins stay masked. NMI
     * still always panics (D-074). */
    if (madt != NULL) {
        uint32_t uid = madtUidFor(madt, id);
        for (uint32_t i = 0; i < madt->lapicNmiCount; i++) {
            const AcpiLapicNmi *n = &madt->lapicNmis[i];
            if ((n->uid != ACPI_LAPIC_NMI_ALL && n->uid != uid) || n->lint > 1) {
                continue;
            }
            uint32_t v = LAPIC_LVT_NMI_MODE;
            if ((n->flags & 3u) == 3u) {
                v |= 1u << 13; /* active low */
            }
            lapicWrite(n->lint == 0 ? LAPIC_REG_LINT0 : LAPIC_REG_LINT1, v);
            if (bsp) {
                klogWrite(KLOG_INFO, "lapic", "LINT%u = NMI (MADT flags=0x%x)", (unsigned)n->lint,
                          (unsigned)n->flags);
            }
        }
    }

    if (id != archCpuApicId()) {
        klogWrite(KLOG_WARN, "lapic", "APIC id %u differs from CPUID's %u", id, archCpuApicId());
    }
    if (bsp) {
        klogWrite(KLOG_INFO, "lapic", "mode=%s id=%u version=0x%02x maxlvt=%u base=0x%llx",
                  x2apic ? "x2apic" : "xapic", id, ver & 0xFFu, maxLvt,
                  (unsigned long long)(base & APIC_BASE_ADDR));
    }
}

void lapicInit(const AcpiMadtInfo *madt) {
    lapicInitCpu(madt, true);
}

void lapicInitAp(const AcpiMadtInfo *madt) {
    lapicInitCpu(madt, false);
}
