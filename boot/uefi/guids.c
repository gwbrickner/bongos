/* Definitions for boot/uefi/include/efi/guids.h. Every protocol GUID is initialized from its
 * own header's *_GUID macro (D-058 keeps those spec-literal), not retyped here -- two copies of
 * the same 16 bytes is exactly how a transcription error hides in one and not the other, which
 * is how EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID's last byte went wrong in this milestone's first
 * draft (tests/host/uefi_guids_test.c is the check that catches it now). The ACPI config-table
 * GUIDs have no header macro to share (ACPI 6.x spec, not UEFI), so they're the one place these
 * bytes are written down. */
#include "include/efi/base.h"
#include "include/efi/graphics-output.h"
#include "include/efi/guids.h"
#include "include/efi/loaded-image.h"
#include "include/efi/rng.h"
#include "include/efi/simple-file-system.h"

const EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
const EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
const EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;
const EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
const EFI_GUID gEfiRngProtocolGuid = EFI_RNG_PROTOCOL_GUID;

/* EFI_ACPI_20_TABLE_GUID */
const EFI_GUID gEfiAcpi20TableGuid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};

/* ACPI_TABLE_GUID (1.0) */
const EFI_GUID gEfiAcpi10TableGuid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
