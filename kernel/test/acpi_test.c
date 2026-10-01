/* ktests for the kernel's ACPI table loading (M3.1, D-166..D-168): the tables are really there
 * after boot, the parsers found what QEMU provides, and every kept table is an intact kernel-memory
 * copy (not a pointer into firmware memory that could be reclaimed). */
#include "acpi.h"
#include "kernel-boot.h"
#include "ktest.h"
#include "kmalloc.h"
#include "vmm.h"

#include <arch/cpu.h>
#include <stdint.h>

KTEST(acpi_tables_loaded) {
    const AcpiTableSet *s = acpiGetTables();
    KTEST_ASSERT(s != NULL);
    KTEST_ASSERT(acpiFindTable("FACP", 0) != NULL);
    KTEST_ASSERT(acpiFindTable("APIC", 0) != NULL);
    KTEST_ASSERT(acpiFindTable("MCFG", 0) != NULL);
    KTEST_ASSERT(acpiFindTable("HPET", 0) != NULL);
    KTEST_ASSERT(acpiFindTable("DSDT", 0) != NULL);
    KTEST_ASSERT(s->dsdtIndex >= 0);
    KTEST_ASSERT_EQ(s->rejected, 0u);
}

/* The BSP's APIC ID: CPUID leaf 0xB's x2APIC ID when that leaf is populated, else leaf 1's. */
static uint32_t bspApicId(void) {
    uint32_t r[4];
    archCpuid(0, 0, r);
    if (r[0] >= 0xB) {
        archCpuid(0xB, 0, r);
        if ((r[1] & 0xFFFF) != 0) {
            return r[3];
        }
    }
    archCpuid(1, 0, r);
    return r[1] >> 24;
}

KTEST(acpi_madt_lists_bsp) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL);
    KTEST_ASSERT(a->madtStatus == STATUS_OK);
    KTEST_ASSERT(a->madt.cpuCount >= 1);
    uint32_t id = bspApicId();
    bool found = false;
    for (uint32_t i = 0; i < a->madt.cpuCount; i++) {
        found = found || a->madt.cpus[i].apicId == id;
    }
    KTEST_ASSERT(found);
    KTEST_ASSERT(a->madt.ioapicCount >= 1);
    KTEST_ASSERT(a->madt.lapicAddress != 0);
}

KTEST(acpi_mcfg_present) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL);
    KTEST_ASSERT(a->mcfgStatus == STATUS_OK);
    KTEST_ASSERT(a->mcfg.count >= 1);
    KTEST_ASSERT(a->mcfg.segs[0].base != 0);
    KTEST_ASSERT_EQ(a->mcfg.segs[0].base & 0xFFFFF, 0u); /* ECAM bases are 1 MiB aligned */
    KTEST_ASSERT(a->mcfg.segs[0].startBus <= a->mcfg.segs[0].endBus);
}

KTEST(acpi_fadt_sane) {
    const AcpiInfo *a = acpiGetInfo();
    KTEST_ASSERT(a != NULL);
    KTEST_ASSERT(a->fadtStatus == STATUS_OK);
    KTEST_ASSERT(a->fadt.sciInt != 0);
    KTEST_ASSERT(a->fadt.pmTimerPresent);
    KTEST_ASSERT(a->fadt.pmTmr.spaceId == 0 || a->fadt.pmTmr.spaceId == 1);
    const AcpiTable *dsdt = acpiFindTable("DSDT", 0);
    KTEST_ASSERT(dsdt != NULL);
    KTEST_ASSERT_EQ(dsdt->phys, a->fadt.dsdtPhys);
}

/* Every kept table must be self-consistent (signature and length as recorded, checksum 0 -- a
 * zero-filled page would also checksum to 0, hence the signature/length checks) and must live in
 * kernel-allocated memory, never in a firmware region the kernel may reclaim or has not mapped. */
KTEST(acpi_tables_are_kernel_copies) {
    const AcpiTableSet *s = acpiGetTables();
    KTEST_ASSERT(s != NULL);
    uint32_t regionCount;
    const BootMemRegion *regions = kernelBootMemMap(&regionCount);
    uint64_t hhdm = kernelBootInfo()->hhdmBase;
    for (uint32_t i = 0; i < s->count; i++) {
        const AcpiTable *t = &s->tables[i];
        KTEST_ASSERT(t->data != NULL);
        KTEST_ASSERT(
            t->data[0] == (uint8_t)t->signature[0] && t->data[1] == (uint8_t)t->signature[1] &&
            t->data[2] == (uint8_t)t->signature[2] && t->data[3] == (uint8_t)t->signature[3]);
        KTEST_ASSERT_EQ(acpiRd32(t->data + 4), t->length);
        KTEST_ASSERT_EQ(acpiChecksum(t->data, t->length), 0u);
        uint64_t first = (uint64_t)(uintptr_t)t->data & ~(uint64_t)0xFFF;
        uint64_t last = ((uint64_t)(uintptr_t)t->data + t->length - 1) & ~(uint64_t)0xFFF;
        for (uint64_t va = first; va <= last; va += 0x1000) {
            uint64_t pa;
            if (va >= hhdm && va - hhdm < BOOTINFO_HHDM_SIZE) {
                pa = va - hhdm;
            } else {
                VmmFlags fl;
                KTEST_ASSERT(vmmLookupKernel(va, &pa, &fl) == STATUS_OK);
            }
            for (uint32_t r = 0; r < regionCount; r++) {
                if (pa >= regions[r].base && pa - regions[r].base < regions[r].length) {
                    KTEST_ASSERT(regions[r].type != BOOT_MEM_ACPI_RECLAIM &&
                                 regions[r].type != BOOT_MEM_ACPI_NVS &&
                                 regions[r].type != BOOT_MEM_RESERVED);
                }
            }
        }
    }
}

KTEST(acpi_parse_rejects_corrupt) {
    const AcpiTable *madt = acpiFindTable("APIC", 0);
    KTEST_ASSERT(madt != NULL);
    uint8_t *copy = kmalloc(madt->length, 0);
    KTEST_ASSERT(copy != NULL);
    for (uint32_t i = 0; i < madt->length; i++) {
        copy[i] = madt->data[i];
    }
    copy[44 + 1] = 0; /* the first entry's length: 0 would loop forever */
    static AcpiMadtInfo info;
    Status st = acpiParseMadt(copy, madt->length, &info);
    kfree(copy);
    KTEST_ASSERT(st == STATUS_ERR_INVALID);
}
