/* ktests for the kernel's ACPI table loading (M3.1, D-166..D-168): the tables are really there
 * after boot, the parsers found what QEMU provides, and every kept table is an intact kernel-memory
 * copy (not a pointer into firmware memory that could be reclaimed). */
#include "acpi.h"
#include "kernel-boot.h"
#include "klog.h"
#include "ktest.h"
#include "kmalloc.h"
#include "vmalloc.h"
#include "page.h"
#include "pmm.h"
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

/* ---- bug-sweeper adversarial ktests (M3.1 step 4 sweep) ---------------------------------- */

static bool kvaContains(uint64_t va) {
    return va >= VM_KVA_BASE && va < VM_KVA_END;
}

static bool hhdmContains(uint64_t va, uint64_t hhdm) {
    return va >= hhdm && va - hhdm < BOOTINFO_HHDM_SIZE;
}

/* 8 KiB is the biggest kept table this test re-reads whole (QEMU's DSDT is 8390 bytes and gets a
 * vmalloc buffer of its own); windows are used for everything else. */
static uint8_t readBuf[16384];

/* acpiKernelReadPhys, the kernel's only path into firmware memory, returns exactly the bytes the
 * loader kept, for every kept table and the RSDP: whole, shifted by one byte at both ends (so the
 * in-page offset is odd and the mapping covers a different page count), and as 2-byte windows over
 * every 4 KiB boundary inside the table. Not vacuous: QEMU's DSDT always spans a page boundary. */
KTEST(acpi_read_phys_matches_copies) {
    const AcpiTableSet *s = acpiGetTables();
    KTEST_ASSERT(s != NULL);
    uint32_t n;
    const BootMemRegion *map = kernelBootMemMap(&n);
    KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, s->rsdpPhys, readBuf, s->rsdpLength), STATUS_OK);
    for (uint32_t i = 0; i < s->rsdpLength; i++) {
        KTEST_ASSERT_EQ(readBuf[i], s->rsdpRaw[i]);
    }
    uint32_t boundaries = 0;
    for (uint32_t ti = 0; ti < s->count; ti++) {
        const AcpiTable *t = &s->tables[ti];
        if (t->length <= sizeof(readBuf)) {
            KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, t->phys, readBuf, t->length), STATUS_OK);
            for (uint32_t i = 0; i < t->length; i++) {
                KTEST_ASSERT_EQ(readBuf[i], t->data[i]);
            }
            KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, t->phys + 1, readBuf, t->length - 2),
                            STATUS_OK);
            for (uint32_t i = 0; i < t->length - 2; i++) {
                KTEST_ASSERT_EQ(readBuf[i], t->data[i + 1]);
            }
        }
        uint64_t b = (t->phys + 0x1000) & ~(uint64_t)0xFFF;
        for (; b < t->phys + t->length; b += 0x1000) {
            uint32_t off = (uint32_t)(b - t->phys);
            KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, b - 1, readBuf, 2), STATUS_OK);
            KTEST_ASSERT_EQ(readBuf[0], t->data[off - 1]);
            KTEST_ASSERT_EQ(readBuf[1], t->data[off]);
            boundaries++;
        }
        KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, t->phys + t->length - 1, readBuf, 1), STATUS_OK);
        KTEST_ASSERT_EQ(readBuf[0], t->data[t->length - 1]);
    }
    KTEST_ASSERT(boundaries >= 1);
}

/* Every temporary window is unmapped and its KVA range returned. The KVA allocator is first-fit
 * and coalesces, so a probe of the same size taken before and after a read lands on the same VA
 * exactly when the read returned its range; that VA must also be unmapped again. */
KTEST(acpi_read_phys_releases_kva) {
    const AcpiTable *dsdt = acpiFindTable("DSDT", 0);
    KTEST_ASSERT(dsdt != NULL);
    uint32_t n;
    const BootMemRegion *map = kernelBootMemMap(&n);
    uint64_t size =
        ((dsdt->phys + dsdt->length + 0xFFF) & ~(uint64_t)0xFFF) - (dsdt->phys & ~(uint64_t)0xFFF);
    KTEST_ASSERT(size >= 0x2000); /* the read really spans pages */
    uint64_t before;
    KTEST_ASSERT_EQ(vmmKvaAlloc(size, &before), STATUS_OK);
    vmmKvaFree(before, size);
    for (int round = 0; round < 64; round++) {
        KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, dsdt->phys, readBuf, dsdt->length), STATUS_OK);
    }
    uint64_t after;
    KTEST_ASSERT_EQ(vmmKvaAlloc(size, &after), STATUS_OK);
    vmmKvaFree(after, size);
    KTEST_ASSERT_EQ(after, before);
    for (uint64_t off = 0; off < size; off += 0x1000) {
        uint64_t pa;
        VmmFlags fl;
        KTEST_ASSERT_EQ(vmmLookupKernel(before + off, &pa, &fl), STATUS_ERR_NOT_FOUND);
    }
}

/* The range policy and the mapping layer both refuse, and a refusal from either leaves no KVA or
 * mapping behind: KERNEL / LOADER_RECLAIM / FRAMEBUFFER regions, a hole above 1 MiB, a wrapping
 * range, a fake RESERVED region past MAXPHYADDR (policy allows, vmmMapKernel refuses), and a fake
 * RESERVED region over the framebuffer (WC HHDM alias: vmmMapKernel's anti-aliasing refuses). A
 * zero-length read at a refused address is OK and writes nothing. */
KTEST(acpi_read_phys_refuses_bad_ranges) {
    uint32_t n;
    const BootMemRegion *map = kernelBootMemMap(&n);
    uint8_t probe[4] = {0xA5, 0xA5, 0xA5, 0xA5};
    uint64_t before;
    KTEST_ASSERT_EQ(vmmKvaAlloc(0x1000, &before), STATUS_OK);
    vmmKvaFree(before, 0x1000);
    bool sawKernel = false;
    uint64_t prevEnd = 0;
    bool sawHole = false;
    for (uint32_t i = 0; i < n; i++) {
        const BootMemRegion *r = &map[i];
        if (r->type == BOOT_MEM_KERNEL || r->type == BOOT_MEM_LOADER_RECLAIM ||
            r->type == BOOT_MEM_FRAMEBUFFER) {
            if (r->base >= 0x100000) {
                KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, r->base, probe, 4), STATUS_ERR_INVALID);
                KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, r->base + r->length - 4, probe, 4),
                                STATUS_ERR_INVALID);
                sawKernel = sawKernel || r->type == BOOT_MEM_KERNEL;
            }
        }
        if (i > 0 && r->base > prevEnd && prevEnd >= 0x100000) {
            KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, prevEnd, probe, 4), STATUS_ERR_INVALID);
            KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, prevEnd - 2, probe, 4), STATUS_ERR_INVALID);
            sawHole = true;
        }
        prevEnd = r->base + r->length;
    }
    KTEST_ASSERT(sawKernel);
    KTEST_ASSERT(sawHole);
    KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, 0xFFFFFFFFFFFFFFF0ull, probe, 0x20),
                    STATUS_ERR_INVALID);
    BootMemRegion high = {1ull << 52, 0x2000, BOOT_MEM_RESERVED, 0};
    KTEST_ASSERT_EQ(acpiKernelReadPhys(&high, 1, (1ull << 52) + 0xFF0, probe, 0x20),
                    STATUS_ERR_INVALID);
    const BootInfo *bi = kernelBootInfo();
    if (bi->fb.phys != 0) {
        BootMemRegion fb = {bi->fb.phys & ~(uint64_t)0xFFF, 0x2000, BOOT_MEM_RESERVED, 0};
        KTEST_ASSERT_EQ(acpiKernelReadPhys(&fb, 1, fb.base + 0xFF0, probe, 0x20),
                        STATUS_ERR_INVALID);
    }
    for (int i = 0; i < 4; i++) {
        KTEST_ASSERT_EQ(probe[i], 0xA5u); /* nothing was copied on any refusal */
    }
    KTEST_ASSERT_EQ(acpiKernelReadPhys(map, n, 0xFFFFFFFFFFFFFFF0ull, probe, 0), STATUS_OK);
    uint64_t after;
    KTEST_ASSERT_EQ(vmmKvaAlloc(0x1000, &after), STATUS_OK);
    vmmKvaFree(after, 0x1000);
    KTEST_ASSERT_EQ(after, before);
}

/* acpiKernelAlloc/acpiKernelFree pick kmalloc for len <= KMALLOC_MAX_SIZE and vmalloc above it, on
 * both sides, at the boundary too (a mismatched free panics through kfree/vfree's misuse checks),
 * every byte of each buffer is usable, and nothing leaks. */
KTEST(acpi_alloc_free_boundary) {
    static const uint32_t lens[] = {
        36, 4096, KMALLOC_MAX_SIZE - 1, KMALLOC_MAX_SIZE, KMALLOC_MAX_SIZE + 1, 65536};
    uint64_t hhdm = kernelBootInfo()->hhdmBase;
    VmallocStats vBefore, vAfter;
    vmallocGetStats(&vBefore);
    SlabCacheStats kBefore, kAfter;
    slabCacheGetStats(kmallocCacheForSize(KMALLOC_MAX_SIZE), &kBefore);
    for (uint32_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        uint32_t len = lens[i];
        uint8_t *p = acpiKernelAlloc(len);
        KTEST_ASSERT(p != NULL);
        uint64_t va = (uint64_t)(uintptr_t)p;
        if (len <= KMALLOC_MAX_SIZE) {
            KTEST_ASSERT(hhdmContains(va, hhdm));
        } else {
            KTEST_ASSERT(kvaContains(va));
        }
        for (uint32_t j = 0; j < len; j++) {
            p[j] = (uint8_t)(j * 13 + i);
        }
        for (uint32_t j = 0; j < len; j++) {
            KTEST_ASSERT_EQ(p[j], (uint8_t)(j * 13 + i));
        }
        acpiKernelFree(p, len);
    }
    vmallocGetStats(&vAfter);
    KTEST_ASSERT_EQ(vAfter.areas, vBefore.areas);
    KTEST_ASSERT_EQ(vAfter.pages, vBefore.pages);
    slabCacheGetStats(kmallocCacheForSize(KMALLOC_MAX_SIZE), &kAfter);
    KTEST_ASSERT_EQ(kAfter.objsAllocated, kBefore.objsAllocated);
}

/* A full second load through the kernel ops (the same path acpiInit took) reproduces the kept set
 * byte for byte, and freeing it returns every vmalloc area and kmalloc object it took. Run twice:
 * the second pass reuses whatever page tables and KVA the first one touched. */
static AcpiTableSet reloadSet;
static Status reloadRead(void *ctx, uint64_t phys, void *dst, uint32_t len) {
    (void)ctx;
    uint32_t n;
    const BootMemRegion *map = kernelBootMemMap(&n);
    return acpiKernelReadPhys(map, n, phys, dst, len);
}
static void *reloadAlloc(void *ctx, uint32_t len) {
    (void)ctx;
    return acpiKernelAlloc(len);
}
static void reloadFree(void *ctx, void *p, uint32_t len) {
    (void)ctx;
    acpiKernelFree(p, len);
}

/* Live objects summed over the kmalloc cache of every kept table's length (a cache counted twice
 * just counts twice on both sides of the comparison). */
static uint64_t kmallocLiveForTables(const AcpiTableSet *s) {
    uint64_t live = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        SlabCache *c = kmallocCacheForSize(s->tables[i].length);
        if (c != NULL) {
            SlabCacheStats st;
            slabCacheGetStats(c, &st);
            live += st.objsAllocated;
        }
    }
    return live;
}

KTEST(acpi_reload_matches_and_frees) {
    const AcpiTableSet *s = acpiGetTables();
    KTEST_ASSERT(s != NULL);
    AcpiPhysOps ops = {NULL, reloadRead, reloadAlloc, reloadFree};
    for (int pass = 0; pass < 2; pass++) {
        VmallocStats vBefore, vAfter;
        vmallocGetStats(&vBefore);
        uint64_t kBefore = kmallocLiveForTables(s);
        KTEST_ASSERT_EQ(acpiTablesLoad(&ops, kernelBootInfo()->rsdpPhys, &reloadSet), STATUS_OK);
        KTEST_ASSERT_EQ(reloadSet.count, s->count);
        KTEST_ASSERT_EQ(reloadSet.dsdtIndex, s->dsdtIndex);
        for (uint32_t i = 0; i < s->count; i++) {
            KTEST_ASSERT_EQ(reloadSet.tables[i].phys, s->tables[i].phys);
            KTEST_ASSERT_EQ(reloadSet.tables[i].length, s->tables[i].length);
            KTEST_ASSERT(reloadSet.tables[i].data != s->tables[i].data);
            for (uint32_t j = 0; j < s->tables[i].length; j++) {
                KTEST_ASSERT_EQ(reloadSet.tables[i].data[j], s->tables[i].data[j]);
            }
        }
        acpiTablesFree(&ops, &reloadSet);
        KTEST_ASSERT_EQ(reloadSet.count, 0u);
        vmallocGetStats(&vAfter);
        KTEST_ASSERT_EQ(vAfter.areas, vBefore.areas);
        KTEST_ASSERT_EQ(vAfter.pages, vBefore.pages);
        KTEST_ASSERT_EQ(kmallocLiveForTables(s), kBefore);
    }
}

/* D-168: ACPI_RECLAIM was freed (after acpiInit copied the tables), ACPI_NVS and RESERVED never
 * were. On BIOS there is no ACPI_RECLAIM at all (SeaBIOS reports its tables RESERVED), which is
 * legitimate: then nothing is checked beyond the count being 0. */
KTEST(acpi_reclaimed) {
    uint32_t count;
    const BootMemRegion *regions = kernelBootMemMap(&count);
    uint64_t expectPages = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t base = regions[i].base;
        uint64_t end = base + regions[i].length;
        if (regions[i].type == BOOT_MEM_ACPI_RECLAIM) {
            if (base < 0x100000) {
                base = 0x100000;
            }
            if (end > BOOTINFO_HHDM_SIZE) {
                end = BOOTINFO_HHDM_SIZE;
            }
            for (uint64_t phys = base; phys < end; phys += 4096) {
                Page *p = pmmPhysToPage(phys);
                KTEST_ASSERT(p != NULL);
                KTEST_ASSERT(p->state != PAGE_STATE_RESERVED);
                expectPages++;
            }
        } else if (regions[i].type == BOOT_MEM_ACPI_NVS) {
            for (uint64_t phys = base; phys < end; phys += 4096) {
                Page *p = pmmPhysToPage(phys);
                KTEST_ASSERT(p == NULL || p->state == PAGE_STATE_RESERVED);
            }
        }
    }
    PmmStats st;
    pmmGetStats(&st);
    KTEST_ASSERT_EQ(st.acpiReclaimedPages, expectPages);
    KTEST_ASSERT(st.reclaimedPages >= st.acpiReclaimedPages);
    if (expectPages == 0) {
        klogWrite(KLOG_INFO, "ktest", "acpi_reclaimed: no ACPI_RECLAIM in this memory map");
    }
}
