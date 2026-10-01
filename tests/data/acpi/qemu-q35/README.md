# QEMU q35 ACPI tables (M3.1, D-169)

Frozen output of `tools/acpiextract` (docs/specs/acpidump.md) for a bongOS boot under QEMU's `q35`
machine, one directory per firmware. Used by `tests/host/kernel_acpi_stored_test.c`. `manifest.txt`
lists each table's physical address, length and file; `RSDP-0.dat` is the RSDP.

| Directory | Firmware | Root table |
|---|---|---|
| `uefi/` | OVMF (`OVMF_CODE_4M.fd`) | RSDP rev 2, XSDT (tables in ACPI_RECLAIM) |
| `bios/` | SeaBIOS (QEMU default) | RSDP rev 0, RSDT only (tables in RESERVED) |

## Provenance
- Captured 2026-10-01 with `qemu-system-x86_64` 8.2.2 (Debian `1:8.2.2+ds-0ubuntu1.18`), `ovmf`
  `2024.02-2ubuntu0.9`, `seabios` `1.16.3-2`, no KVM (TCG).
- Command: `make` (the ktest image carries `acpidump=1`), then
  `tests/harness/run-qemu.sh --image build/bongos-ktest.img --fw <uefi|bios> --cpus 4 --timeout 600
  --name acpi-capture-<fw>` (which runs `-M q35 -cpu max -smp 4 -m 512`), then
  `build/tools/acpiextract/acpiextract -o tests/data/acpi/qemu-q35/<fw> build/logs/acpi-capture-<fw>.serial.log`.
- `-smp 4` gives the MADT four CPUs. No `-device amd-iommu`, so there is no IVRS here.

## Rules
The set is frozen. Host tests assert semantic fields only (CPU count, APIC IDs, MCFG base, PM timer
layout, ...), never whole-file hashes, checksums or firmware-chosen addresses, so regenerating with
the same QEMU settings and a newer QEMU should only need the expectations in the test updated if a
real value changed. These are firmware-generated blobs from QEMU/OVMF/SeaBIOS (including the DSDT
AML); they are test data only and not part of the base system.
