# ACPIDUMP v1 (serial dump of the kernel's ACPI tables)

Written by the kernel (`kernel/drivers/acpi/acpi-dump.c`) when the command line contains
`acpidump=1`, and read by `tools/acpiextract`. Decision: D-169.

**Transport.** Lines go raw to COM1 (`serialWriteString`, so `\n` becomes `\r\n`), not through klog.
Readers strip one trailing `\r`. Lines that do not start with `ACPIDUMP ` are ignored, even inside
a block.

**Grammar** (in this order):
```
ACPIDUMP BEGIN v1 rsdp=0x<16 lowercase hex>
  per table, in order: RSDP, root table, root entries in root order, DSDT
ACPIDUMP TABLE <SIG4> phys=0x<16 hex> len=<decimal>
ACPIDUMP <8 hex offset> <2..64 lowercase hex chars>   32 bytes per line, offset a multiple of 32;
                                                      only the last data line may be shorter
ACPIDUMP TABLE-END <SIG4>
ACPIDUMP END tables=<decimal: number of TABLE blocks, RSDP included>
```
`SIG4`: the RSDP block uses `RSDP`. Otherwise the four signature bytes with each byte outside
`[A-Za-z0-9]` replaced by `_`; the true signature is always the first 4 data bytes. The RSDP
block's `len` is 20 (revision 0/1) or its Length field (revision 2+).

**Reader rules.** Use the last BEGIN..END block (earlier ones are ignored); it must be complete. Data offsets are contiguous from 0 and the
byte total equals `len`. The END count matches the number of TABLE blocks. Checksums: the RSDP over
20 bytes (and over `len` when `len >= 36`); every other table over `len`. Any violation is an error
that names the line number.

**acpiextract output.** `DIR/<SIG4>-<n>.dat` (`n` = 0-based occurrence index per SIG4 in dump order)
and `DIR/manifest.txt`:
```
# acpiextract manifest v1
rsdp 0x<16hex>
<file> <SIG4> 0x<16hex phys> <len>        one per table, dump order
```
