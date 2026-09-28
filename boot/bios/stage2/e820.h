/* E820 memory map scan (INT 15h AX=E820h, "ACPI System Address Map Interfaces", D-107), via the
 * real-mode thunk. */
#ifndef BOOT_BIOS_STAGE2_E820_H
#define BOOT_BIOS_STAGE2_E820_H

#include <stdint.h>

#include "memmap.h"

/* Scans the E820 map, converting each usable descriptor via memMapE820TypeToBootMem() into
 * `out` (capacity `outCap`). Stops on the first failed call (CF set, or EAX not echoed back as
 * 'SMAP' -- the same handling for the first call and any later one: whatever was collected so
 * far is still used), on EBX wrapping back to 0 (a normal end of list), or once `outCap` entries
 * have been written (the caller can tell this happened by comparing the return value to
 * `outCap`). A descriptor is skipped if its returned length is less than 20 bytes, its region
 * length is 0, or (when it carries the ACPI 3.0 extended-attribute dword) that dword's bit 0
 * ("valid") is clear. Bounded to at most 256 real-mode calls regardless of `outCap`, so a BIOS
 * that never sets EBX back to 0 can't hang stage2. Returns the number of entries written to
 * `out`. */
uint32_t e820Scan(MemMapInput *out, uint32_t outCap);

#endif
