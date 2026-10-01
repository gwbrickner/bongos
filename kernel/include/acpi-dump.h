/* ACPIDUMP v1 formatter (docs/specs/acpidump.md, D-169): renders a loaded AcpiTableSet as the
 * serial text tools/acpiextract reads back. Pure: lines go out through a callback, so the kernel
 * can send them straight to the UART (never klog: it would flood fbcon) and host tests can
 * collect them into a string. */
#ifndef KERNEL_ACPI_DUMP_H
#define KERNEL_ACPI_DUMP_H

#include "acpi-tables.h"

/* Receives one complete line, without a trailing newline, NUL-terminated. */
typedef void (*AcpiDumpEmit)(void *ctx, const char *line);

/* Emits `ACPIDUMP BEGIN`, a block for the RSDP, one block per table in `s` (set order), and
 * `ACPIDUMP END`. No locks, no allocation, no failure modes; pure apart from `emit`. */
void acpiDumpTables(const AcpiTableSet *s, AcpiDumpEmit emit, void *ctx);

#endif
