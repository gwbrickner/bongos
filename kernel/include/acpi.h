/* Kernel-side ACPI table access (ARCHITECTURE §14, D-166..D-168, ROADMAP M3.1): finds the RSDP
 * BootInfo reports, copies every valid table into kernel memory through temporary read-only KVA
 * mappings, and parses the FADT/MADT/MCFG/HPET/IVRS. The pure loader and parsers are in
 * acpi-tables.h; this is the glue that touches firmware memory, klog and the allocators. */
#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include "acpi-tables.h"
#include "bootinfo.h"

/* Loads and logs the ACPI tables, parses them, and (when `cmdline` has the token `acpidump=1`)
 * dumps them raw over serial (docs/specs/acpidump.md). Returns OK, NOT_FOUND (the loader reported
 * no RSDP), INVALID (no usable RSDP/root) or NO_MEMORY; on a non-OK return nothing is kept and the
 * accessors below return NULL. On OK no mapping of, and no pointer into, firmware memory survives:
 * every table is a kmalloc/vmalloc copy kept for the kernel's lifetime. Must be called once, after
 * vmallocInit() and before any AP exists (the temporary KVA mappings are flushed on this CPU only).
 * Boot-time only, BSP, IF=0. Locks: none of its own (vmmLock and pmmLock are taken by callees in
 * the D-088 order). May not sleep. Never panics on firmware data, only on kernel bugs. */
Status acpiInit(const BootInfo *bi, const BootMemRegion *map, uint32_t mapCount,
                const char *cmdline);

/* The parsed tables / table copies, or NULL unless acpiInit() returned OK. No locks; read-only
 * after acpiInit(). */
const AcpiInfo *acpiGetInfo(void);
const AcpiTableSet *acpiGetTables(void);

/* The `instance`-th kept table with signature `sig` (e.g. "DSDT", "SSDT" for the future AML
 * interpreter), or NULL. */
const AcpiTable *acpiFindTable(const char sig[4], uint32_t instance);

#endif
