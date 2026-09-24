/* EFI_LOADED_IMAGE_PROTOCOL, UEFI Spec §9.1. HandleProtocol/OpenProtocol on the loader's own
 * ImageHandle returns this; M1.3 uses DeviceHandle to open the volume the loader itself came
 * from (so it can find /bong/kernel.elf on the same ESP without hardcoding a device path). */
#ifndef EFI_LOADED_IMAGE_H
#define EFI_LOADED_IMAGE_H

#include "base.h"
#include "boot-services.h"
#include "system-table.h"

#define EFI_LOADED_IMAGE_PROTOCOL_GUID                                                             \
    {                                                                                              \
        0x5b1b31a1, 0x9562, 0x11d2, {                                                              \
            0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b                                         \
        }                                                                                          \
    }

#define EFI_LOADED_IMAGE_PROTOCOL_REVISION 0x1000

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;

    EFI_HANDLE DeviceHandle;
    VOID *FilePath; /* EFI_DEVICE_PATH_PROTOCOL*; opaque until a device-path header exists */
    VOID *Reserved;

    UINT32 LoadOptionsSize;
    VOID *LoadOptions;

    VOID *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_STATUS(EFIAPI *Unload)(IN EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

#endif
