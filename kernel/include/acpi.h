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

/* The AcpiPhysOps pieces acpiInit() hands the pure loader, exposed so ktests can drive them
 * directly (and a later caller could reload tables the same way).
 * acpiKernelReadPhys copies [phys, phys+len) out of firmware memory through a temporary read-only
 * WB KVA mapping of the covering pages (the HHDM does not map RESERVED memory, where BIOS keeps the
 * RSDP and every table); the mapping and its KVA range are gone again before it returns. `len` 0
 * is OK and touches nothing. Returns INVALID if acpiPhysRangeAllowed(map, mapCount, ...) refuses
 * the range or vmmMapKernel refuses the pages (e.g. past MAXPHYADDR, or a WC HHDM alias), or
 * NO_MEMORY if no KVA range or page-table page is available. Panics only if unmapping its own
 * mapping fails (a kernel bug). Locks: none of its own (vmmLock/pmmLock in callees, D-088 order).
 * BSP before any AP exists (the TLB flush is local). May not sleep.
 * acpiKernelAlloc returns `len` (nonzero) bytes from kmalloc up to KMALLOC_MAX_SIZE, else vmalloc,
 * or NULL on OOM; acpiKernelFree(p, len) must get the same `len`, which picks the same allocator.
 * Same locking/sleep rules as kmalloc/vmalloc. */
Status acpiKernelReadPhys(const BootMemRegion *map, uint32_t mapCount, uint64_t phys, void *dst,
                          uint32_t len);
void *acpiKernelAlloc(uint32_t len);
void acpiKernelFree(void *p, uint32_t len);

#endif
