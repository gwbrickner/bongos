/* Test fixture for the stored ACPI tables (tests/data/acpi/qemu-q35/<fw>/, M3.1, D-169): loads a
 * tools/acpiextract output directory (manifest.txt + <SIG4>-<n>.dat) into a fake physical memory
 * and exposes it through AcpiPhysOps, with an allocation counter every test must see return to 0.
 */
#ifndef TESTS_HOST_ACPI_FIXTURE_H
#define TESTS_HOST_ACPI_FIXTURE_H

#include "acpi-tables.h"

#include <stdint.h>

#define ACPI_FX_MAX_BLOCKS 64

typedef struct {
    uint64_t phys;
    uint32_t len;
    uint8_t *bytes;
    char sig[5];
} AcpiFxBlock;

typedef struct {
    AcpiFxBlock blocks[ACPI_FX_MAX_BLOCKS]; /* manifest order: RSDP first */
    int count;
    uint64_t rsdpPhys;
    int live; /* outstanding allocations made through ops */
} AcpiFx;

/* Loads `dir`. Returns 0 on success, -1 if the directory or a file is missing/malformed. */
int acpiFxLoad(AcpiFx *fx, const char *dir);
void acpiFxRelease(AcpiFx *fx);
AcpiPhysOps acpiFxOps(AcpiFx *fx);

#endif
