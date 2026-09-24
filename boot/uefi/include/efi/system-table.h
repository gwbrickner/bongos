/* EFI_SYSTEM_TABLE, UEFI Spec §4.3. The single struct every EFI application receives; ConOut
 * (for the M1.2 banner) and BootServices/ConfigurationTable (RSDP lookup, memory map, protocol
 * location -- M1.3/M1.4) all hang off this. */
#ifndef EFI_SYSTEM_TABLE_H
#define EFI_SYSTEM_TABLE_H

#include "base.h"
#include "boot-services.h"
#include "runtime-services.h"
#include "simple-text-output.h"

#define EFI_SYSTEM_TABLE_SIGNATURE 0x5453595320494249ULL /* "IBI SYST" */

/* §4.6. Config table entries the loader looks up by GUID (guids.h): the ACPI RSDP today, more
 * later. */
typedef struct {
    EFI_GUID VendorGuid;
    VOID *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;

    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;

    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;

    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;

    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;

    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;

    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif
