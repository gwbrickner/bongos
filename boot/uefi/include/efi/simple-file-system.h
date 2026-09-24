/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL + EFI_FILE_PROTOCOL, UEFI Spec §13.4-13.5. Needed from M1.3 on
 * to read /bong/boot.cfg, kernel.elf, and initrd.img off the ESP. */
#ifndef EFI_SIMPLE_FILE_SYSTEM_H
#define EFI_SIMPLE_FILE_SYSTEM_H

#include "base.h"

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID                                                       \
    {                                                                                              \
        0x964e5b22, 0x6459, 0x11d2, {                                                              \
            0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b                                         \
        }                                                                                          \
    }
#define EFI_FILE_INFO_ID                                                                           \
    {                                                                                              \
        0x09576e92, 0x6d3f, 0x11d2, {                                                              \
            0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b                                         \
        }                                                                                          \
    }

#define EFI_FILE_PROTOCOL_REVISION 0x00010000

/* Open modes and attributes, §13.5. */
#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

#define EFI_FILE_READ_ONLY 0x0000000000000001ULL
#define EFI_FILE_HIDDEN    0x0000000000000002ULL
#define EFI_FILE_SYSTEM    0x0000000000000004ULL
#define EFI_FILE_DIRECTORY 0x0000000000000010ULL
#define EFI_FILE_ARCHIVE   0x0000000000000020ULL

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_FILE_OPEN)(IN EFI_FILE_PROTOCOL *This,
                                          OUT EFI_FILE_PROTOCOL **NewHandle, IN CHAR16 *FileName,
                                          IN UINT64 OpenMode, IN UINT64 Attributes);
typedef EFI_STATUS(EFIAPI *EFI_FILE_CLOSE)(IN EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS(EFIAPI *EFI_FILE_DELETE)(IN EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS(EFIAPI *EFI_FILE_READ)(IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize,
                                          OUT VOID *Buffer);
typedef EFI_STATUS(EFIAPI *EFI_FILE_WRITE)(IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize,
                                           IN VOID *Buffer);
typedef EFI_STATUS(EFIAPI *EFI_FILE_GET_POSITION)(IN EFI_FILE_PROTOCOL *This, OUT UINT64 *Position);
typedef EFI_STATUS(EFIAPI *EFI_FILE_SET_POSITION)(IN EFI_FILE_PROTOCOL *This, IN UINT64 Position);
typedef EFI_STATUS(EFIAPI *EFI_FILE_GET_INFO)(IN EFI_FILE_PROTOCOL *This,
                                              IN EFI_GUID *InformationType,
                                              IN OUT UINTN *BufferSize, OUT VOID *Buffer);
typedef EFI_STATUS(EFIAPI *EFI_FILE_SET_INFO)(IN EFI_FILE_PROTOCOL *This,
                                              IN EFI_GUID *InformationType, IN UINTN BufferSize,
                                              IN VOID *Buffer);
typedef EFI_STATUS(EFIAPI *EFI_FILE_FLUSH)(IN EFI_FILE_PROTOCOL *This);

struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    EFI_FILE_DELETE Delete;
    EFI_FILE_READ Read;
    EFI_FILE_WRITE Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    EFI_FILE_SET_INFO SetInfo;
    EFI_FILE_FLUSH Flush;
    /* OpenEx/ReadEx/WriteEx/FlushEx (async I/O, revision 2) omitted: the boot.cfg/kernel/initrd
     * reads M1.3 adds are synchronous, so nothing needs them yet. */
};

/* §13.5 EFI_FILE_INFO. A variable-length struct: FileName is a NUL-terminated CHAR16[] that
 * continues past the declared single-element array, per spec. Callers size their buffer with
 * GetInfo's EFI_BUFFER_TOO_SMALL/BufferSize round-trip, never sizeof(EFI_FILE_INFO) alone. */
typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT64 CreateTimeRaw[2]; /* EFI_TIME is defined in runtime-services.h; kept as raw
                                 bytes here so this header doesn't need to pull that one in
                                 for the one field the loader doesn't use */
    UINT64 LastAccessTimeRaw[2];
    UINT64 ModificationTimeRaw[2];
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME)(
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, OUT EFI_FILE_PROTOCOL **Root);

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME OpenVolume;
};

#endif
