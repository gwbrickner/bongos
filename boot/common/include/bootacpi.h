/* RSDP scan (ACPI "Finding the RSDP on IA-PC Systems", D-107), the BIOS-path equivalent of UEFI's
 * config-table lookup (boot/uefi/handoff.c's handoffFindRsdp). */
#ifndef BOOT_COMMON_BOOTACPI_H
#define BOOT_COMMON_BOOTACPI_H

#include <stddef.h>
#include <stdint.h>

/* Scans `buf` (`len` bytes, representing physical memory starting at `base`) on 16-byte-aligned
 * steps for a checksum-valid RSDP: the 8-byte "RSD PTR " signature, then a whole-structure
 * checksum (the first 20 bytes must sum to 0 mod 256 -- ACPI 1.0); if `revision >= 2`, also
 * checks 36 <= Length <= (bytes remaining in `buf` from that offset) and a second checksum over
 * the first `Length` bytes. Returns the RSDP's physical address (`base` + the matching offset) on
 * success, 0 if nothing validates. Pure: takes a plain byte buffer rather than reading physical
 * memory directly, so this is host-testable with a synthetic buffer -- the caller passes the real
 * EBDA/0xE0000-0xFFFFF windows via bootPhysToPtr(). No locks, boot-time or host-test only. */
uint64_t bootAcpiScanForRsdp(const uint8_t *buf, size_t len, uint64_t base);

#endif
