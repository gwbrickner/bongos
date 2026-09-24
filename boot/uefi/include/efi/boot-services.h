/* EFI_BOOT_SERVICES, UEFI Spec §4.4 / §7. Only the members the loader calls across M1.2-M1.3
 * (memory allocation, protocol location, and ExitBootServices) are exercised yet; the rest of
 * the struct is still declared in full so its layout -- and therefore every offset after it --
 * matches the spec exactly. */
#ifndef EFI_BOOT_SERVICES_H
#define EFI_BOOT_SERVICES_H

#include "base.h"

#define EFI_BOOT_SERVICES_SIGNATURE 0x56524553544f4f42ULL /* "BOOTSERV" */

/* §7.2 EFI_ALLOCATE_TYPE */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* §7.2 EFI_MEMORY_TYPE */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

/* §7.2 EFI_MEMORY_DESCRIPTOR. Version is EFI_MEMORY_DESCRIPTOR_VERSION below; the descriptor
 * size returned by GetMemoryMap can exceed sizeof(EFI_MEMORY_DESCRIPTOR), so callers must
 * always stride by the returned DescriptorSize, never sizeof(EFI_MEMORY_DESCRIPTOR). */
#define EFI_MEMORY_DESCRIPTOR_VERSION 1
typedef struct {
    UINT32 Type;
    UINT32 Pad; /* not part of the spec struct; keeps PhysicalStart 8-byte aligned like real fw */
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum { TimerCancel, TimerPeriodic, TimerRelative, TimerTypeMax } EFI_TIMER_DELAY;

typedef enum { AllHandles, ByRegisterNotify, ByProtocol, SearchTypeMax } EFI_LOCATE_SEARCH_TYPE;

typedef enum { EfiNativeInterface, EfiPcodeInterface } EFI_INTERFACE_TYPE;

/* TPL levels used by RaiseTPL/RestoreTPL, §7.1.1. */
#define TPL_APPLICATION 4
#define TPL_CALLBACK    8
#define TPL_NOTIFY      16
#define TPL_HIGH_LEVEL  31

/* Event type flags for CreateEvent, §7.1.2. */
#define EVT_TIMER                         0x80000000
#define EVT_RUNTIME                       0x40000000
#define EVT_NOTIFY_WAIT                   0x00000100
#define EVT_NOTIFY_SIGNAL                 0x00000200
#define EVT_SIGNAL_EXIT_BOOT_SERVICES     0x00000201
#define EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE 0x60000202

typedef VOID(EFIAPI *EFI_EVENT_NOTIFY)(IN EFI_EVENT Event, IN VOID *Context);

/* UEFI Spec §7.3 EFI_OPEN_PROTOCOL_INFORMATION_ENTRY. */
typedef struct {
    EFI_HANDLE AgentHandle;
    EFI_HANDLE ControllerHandle;
    UINT32 Attributes;
    UINT32 OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY; /* only referenced by OpenProtocolInformation's signature
                                          below; not called anywhere yet */

/* Task priority level. */
typedef EFI_TPL(EFIAPI *EFI_RAISE_TPL)(IN EFI_TPL NewTpl);
typedef VOID(EFIAPI *EFI_RESTORE_TPL)(IN EFI_TPL OldTpl);

/* Memory. */
typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_PAGES)(IN EFI_ALLOCATE_TYPE Type,
                                               IN EFI_MEMORY_TYPE MemoryType, IN UINTN Pages,
                                               IN OUT EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS(EFIAPI *EFI_FREE_PAGES)(IN EFI_PHYSICAL_ADDRESS Memory, IN UINTN Pages);
typedef EFI_STATUS(EFIAPI *EFI_GET_MEMORY_MAP)(IN OUT UINTN *MemoryMapSize,
                                               OUT EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                               OUT UINTN *MapKey, OUT UINTN *DescriptorSize,
                                               OUT UINT32 *DescriptorVersion);
typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_POOL)(IN EFI_MEMORY_TYPE PoolType, IN UINTN Size,
                                              OUT VOID **Buffer);
typedef EFI_STATUS(EFIAPI *EFI_FREE_POOL)(IN VOID *Buffer);

/* Events and timers. */
typedef EFI_STATUS(EFIAPI *EFI_CREATE_EVENT)(IN UINT32 Type, IN EFI_TPL NotifyTpl,
                                             IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
                                             IN VOID *NotifyContext OPTIONAL, OUT EFI_EVENT *Event);
typedef EFI_STATUS(EFIAPI *EFI_SET_TIMER)(IN EFI_EVENT Event, IN EFI_TIMER_DELAY Type,
                                          IN UINT64 TriggerTime);
typedef EFI_STATUS(EFIAPI *EFI_WAIT_FOR_EVENT)(IN UINTN NumberOfEvents, IN EFI_EVENT *Event,
                                               OUT UINTN *Index);
typedef EFI_STATUS(EFIAPI *EFI_SIGNAL_EVENT)(IN EFI_EVENT Event);
typedef EFI_STATUS(EFIAPI *EFI_CLOSE_EVENT)(IN EFI_EVENT Event);
typedef EFI_STATUS(EFIAPI *EFI_CHECK_EVENT)(IN EFI_EVENT Event);

/* Protocol handler services. */
typedef EFI_STATUS(EFIAPI *EFI_INSTALL_PROTOCOL_INTERFACE)(IN OUT EFI_HANDLE *Handle,
                                                           IN EFI_GUID *Protocol,
                                                           IN EFI_INTERFACE_TYPE InterfaceType,
                                                           IN VOID *Interface);
typedef EFI_STATUS(EFIAPI *EFI_REINSTALL_PROTOCOL_INTERFACE)(IN EFI_HANDLE Handle,
                                                             IN EFI_GUID *Protocol,
                                                             IN VOID *OldInterface,
                                                             IN VOID *NewInterface);
typedef EFI_STATUS(EFIAPI *EFI_UNINSTALL_PROTOCOL_INTERFACE)(IN EFI_HANDLE Handle,
                                                             IN EFI_GUID *Protocol,
                                                             IN VOID *Interface);
typedef EFI_STATUS(EFIAPI *EFI_HANDLE_PROTOCOL)(IN EFI_HANDLE Handle, IN EFI_GUID *Protocol,
                                                OUT VOID **Interface);
typedef EFI_STATUS(EFIAPI *EFI_REGISTER_PROTOCOL_NOTIFY)(IN EFI_GUID *Protocol, IN EFI_EVENT Event,
                                                         OUT VOID **Registration);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_HANDLE)(IN EFI_LOCATE_SEARCH_TYPE SearchType,
                                              IN EFI_GUID *Protocol OPTIONAL,
                                              IN VOID *SearchKey OPTIONAL, IN OUT UINTN *BufferSize,
                                              OUT EFI_HANDLE *Buffer);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_DEVICE_PATH)(IN EFI_GUID *Protocol, IN OUT VOID **DevicePath,
                                                   OUT EFI_HANDLE *Device);
typedef EFI_STATUS(EFIAPI *EFI_INSTALL_CONFIGURATION_TABLE)(IN EFI_GUID *Guid, IN VOID *Table);

/* Image services. */
typedef EFI_STATUS(EFIAPI *EFI_IMAGE_LOAD)(IN BOOLEAN BootPolicy, IN EFI_HANDLE ParentImageHandle,
                                           IN VOID *DevicePath OPTIONAL,
                                           IN VOID *SourceBuffer OPTIONAL, IN UINTN SourceSize,
                                           OUT EFI_HANDLE *ImageHandle);
typedef EFI_STATUS(EFIAPI *EFI_IMAGE_START)(IN EFI_HANDLE ImageHandle, OUT UINTN *ExitDataSize,
                                            OUT CHAR16 **ExitData OPTIONAL);
typedef EFI_STATUS(EFIAPI *EFI_EXIT)(IN EFI_HANDLE ImageHandle, IN EFI_STATUS ExitStatus,
                                     IN UINTN ExitDataSize, IN CHAR16 *ExitData OPTIONAL);
typedef EFI_STATUS(EFIAPI *EFI_IMAGE_UNLOAD)(IN EFI_HANDLE ImageHandle);
typedef EFI_STATUS(EFIAPI *EFI_EXIT_BOOT_SERVICES)(IN EFI_HANDLE ImageHandle, IN UINTN MapKey);

/* Miscellaneous. */
typedef EFI_STATUS(EFIAPI *EFI_GET_NEXT_MONOTONIC_COUNT)(OUT UINT64 *Count);
typedef EFI_STATUS(EFIAPI *EFI_STALL)(IN UINTN Microseconds);
typedef EFI_STATUS(EFIAPI *EFI_SET_WATCHDOG_TIMER)(IN UINTN Timeout, IN UINT64 WatchdogCode,
                                                   IN UINTN DataSize,
                                                   IN CHAR16 *WatchdogData OPTIONAL);

/* Driver support. */
typedef EFI_STATUS(EFIAPI *EFI_CONNECT_CONTROLLER)(IN EFI_HANDLE ControllerHandle,
                                                   IN EFI_HANDLE *DriverImageHandle OPTIONAL,
                                                   IN VOID *RemainingDevicePath OPTIONAL,
                                                   IN BOOLEAN Recursive);
typedef EFI_STATUS(EFIAPI *EFI_DISCONNECT_CONTROLLER)(IN EFI_HANDLE ControllerHandle,
                                                      IN EFI_HANDLE DriverImageHandle OPTIONAL,
                                                      IN EFI_HANDLE ChildHandle OPTIONAL);

/* Open/close protocol, §7.3. */
#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL  0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL        0x00000002
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL       0x00000004
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x00000008
#define EFI_OPEN_PROTOCOL_BY_DRIVER           0x00000010
#define EFI_OPEN_PROTOCOL_EXCLUSIVE           0x00000020

typedef EFI_STATUS(EFIAPI *EFI_OPEN_PROTOCOL)(IN EFI_HANDLE Handle, IN EFI_GUID *Protocol,
                                              OUT VOID **Interface OPTIONAL,
                                              IN EFI_HANDLE AgentHandle,
                                              IN EFI_HANDLE ControllerHandle, IN UINT32 Attributes);
typedef EFI_STATUS(EFIAPI *EFI_CLOSE_PROTOCOL)(IN EFI_HANDLE Handle, IN EFI_GUID *Protocol,
                                               IN EFI_HANDLE AgentHandle,
                                               IN EFI_HANDLE ControllerHandle);
typedef EFI_STATUS(EFIAPI *EFI_OPEN_PROTOCOL_INFORMATION)(
    IN EFI_HANDLE Handle, IN EFI_GUID *Protocol,
    OUT EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer, OUT UINTN *EntryCount);
typedef EFI_STATUS(EFIAPI *EFI_PROTOCOLS_PER_HANDLE)(IN EFI_HANDLE Handle,
                                                     OUT EFI_GUID ***ProtocolBuffer,
                                                     OUT UINTN *ProtocolBufferCount);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(IN EFI_LOCATE_SEARCH_TYPE SearchType,
                                                     IN EFI_GUID *Protocol OPTIONAL,
                                                     IN VOID *SearchKey OPTIONAL,
                                                     OUT UINTN *NoHandles, OUT EFI_HANDLE **Buffer);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_PROTOCOL)(IN EFI_GUID *Protocol,
                                                IN VOID *Registration OPTIONAL,
                                                OUT VOID **Interface);
typedef EFI_STATUS(EFIAPI *EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)(IN OUT EFI_HANDLE *Handle,
                                                                     ...);
typedef EFI_STATUS(EFIAPI *EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)(IN EFI_HANDLE Handle, ...);

typedef EFI_STATUS(EFIAPI *EFI_CALCULATE_CRC32)(IN VOID *Data, IN UINTN DataSize,
                                                OUT UINT32 *Crc32);

typedef VOID(EFIAPI *EFI_COPY_MEM)(IN VOID *Destination, IN VOID *Source, IN UINTN Length);
typedef VOID(EFIAPI *EFI_SET_MEM)(IN VOID *Buffer, IN UINTN Size, IN UINT8 Value);

typedef EFI_STATUS(EFIAPI *EFI_CREATE_EVENT_EX)(IN UINT32 Type, IN EFI_TPL NotifyTpl,
                                                IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
                                                IN CONST VOID *NotifyContext OPTIONAL,
                                                IN CONST EFI_GUID *EventGroup OPTIONAL,
                                                OUT EFI_EVENT *Event);

/* §4.4.1. Field order matches the spec exactly: an offset error here silently corrupts every
 * call after the mistake. */
typedef struct {
    EFI_TABLE_HEADER Hdr;

    EFI_RAISE_TPL RaiseTPL;
    EFI_RESTORE_TPL RestoreTPL;

    EFI_ALLOCATE_PAGES AllocatePages;
    EFI_FREE_PAGES FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;

    EFI_CREATE_EVENT CreateEvent;
    EFI_SET_TIMER SetTimer;
    EFI_WAIT_FOR_EVENT WaitForEvent;
    EFI_SIGNAL_EVENT SignalEvent;
    EFI_CLOSE_EVENT CloseEvent;
    EFI_CHECK_EVENT CheckEvent;

    EFI_INSTALL_PROTOCOL_INTERFACE InstallProtocolInterface;
    EFI_REINSTALL_PROTOCOL_INTERFACE ReinstallProtocolInterface;
    EFI_UNINSTALL_PROTOCOL_INTERFACE UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    VOID *Reserved;
    EFI_REGISTER_PROTOCOL_NOTIFY RegisterProtocolNotify;
    EFI_LOCATE_HANDLE LocateHandle;
    EFI_LOCATE_DEVICE_PATH LocateDevicePath;
    EFI_INSTALL_CONFIGURATION_TABLE InstallConfigurationTable;

    EFI_IMAGE_LOAD LoadImage;
    EFI_IMAGE_START StartImage;
    EFI_EXIT Exit;
    EFI_IMAGE_UNLOAD UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;

    EFI_GET_NEXT_MONOTONIC_COUNT GetNextMonotonicCount;
    EFI_STALL Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;

    EFI_CONNECT_CONTROLLER ConnectController;
    EFI_DISCONNECT_CONTROLLER DisconnectController;

    EFI_OPEN_PROTOCOL OpenProtocol;
    EFI_CLOSE_PROTOCOL CloseProtocol;
    EFI_OPEN_PROTOCOL_INFORMATION OpenProtocolInformation;

    EFI_PROTOCOLS_PER_HANDLE ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES InstallMultipleProtocolInterfaces;
    EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES UninstallMultipleProtocolInterfaces;

    EFI_CALCULATE_CRC32 CalculateCrc32;

    EFI_COPY_MEM CopyMem;
    EFI_SET_MEM SetMem;
    EFI_CREATE_EVENT_EX CreateEventEx;
} EFI_BOOT_SERVICES;

#endif
