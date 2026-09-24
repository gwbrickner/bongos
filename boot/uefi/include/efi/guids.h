/* Protocol and configuration-table GUIDs the loader looks up. Declared extern here and defined
 * once in boot/uefi/guids.c: a GUID used in more than one translation unit must have exactly one
 * definition, and every consumer needs the same address-stable object to pass to
 * LocateProtocol/InstallConfigurationTable lookups. */
#ifndef EFI_GUIDS_H
#define EFI_GUIDS_H

#include "base.h"

extern const EFI_GUID gEfiLoadedImageProtocolGuid;
extern const EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern const EFI_GUID gEfiFileInfoGuid;
extern const EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern const EFI_GUID gEfiRngProtocolGuid;

/* ACPI config-table GUIDs (UEFI Spec §4.6), for finding the RSDP: ARCHITECTURE §5.5 step 6 tries
 * ACPI 2.0 first, then falls back to ACPI 1.0. */
extern const EFI_GUID gEfiAcpi20TableGuid;
extern const EFI_GUID gEfiAcpi10TableGuid;

#endif
