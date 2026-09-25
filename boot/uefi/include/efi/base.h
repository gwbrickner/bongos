/* Minimal UEFI base types (UEFI Spec 2.10 §2.3), written from the spec (D-007): no gnu-efi, no
 * POSIX-UEFI. Only what the loader actually needs is added; new protocols get their own header
 * under boot/uefi/include/efi/ as later milestones need them. */
#ifndef EFI_BASE_H
#define EFI_BASE_H

#include <stdbool.h>
#include <stddef.h> /* NULL */
#include <stdint.h>

/* The loader is built with --target=x86_64-unknown-windows (ARCHITECTURE §3), which already
 * uses the Microsoft x64 calling convention for every function, so EFIAPI adds nothing there.
 * It stays as a marker (matching every other UEFI header in existence) in case a non-Windows
 * host target is ever used to build this code, where it would need to expand to
 * __attribute__((ms_abi)). */
#define EFIAPI

typedef uint8_t BOOLEAN;
#define TRUE  1
#define FALSE 0

typedef int8_t INT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef int64_t INTN; /* native width; x86_64 only (ARCHITECTURE §1.3) */
typedef uint64_t UINTN;
typedef uint8_t CHAR8;
typedef uint16_t CHAR16; /* UCS-2, not UTF-16: no surrogate pairs in firmware text */
typedef void VOID;

typedef UINTN EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;
typedef UINTN EFI_TPL;
typedef UINT64 EFI_LBA;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

/* Parameter-direction annotations: documentation only in C, but they mirror the spec text so a
 * header reads the same way the spec does. */
#define IN
#define OUT
#define OPTIONAL
#define CONST const

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

/* EFI_STATUS: bit 63 (the sign bit of a 64-bit UINTN) set means error; EFI_WARNING codes are
 * small positive numbers; EFI_SUCCESS is 0. §2.3.1. */
#define EFI_ERROR_BIT     0x8000000000000000ULL
#define EFI_ERROR(status) (((INTN)(status)) < 0)

#define EFI_SUCCESS            0ULL
#define EFI_LOAD_ERROR         (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER  (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED        (EFI_ERROR_BIT | 3)
#define EFI_BAD_BUFFER_SIZE    (EFI_ERROR_BIT | 4)
#define EFI_BUFFER_TOO_SMALL   (EFI_ERROR_BIT | 5)
#define EFI_NOT_READY          (EFI_ERROR_BIT | 6)
#define EFI_DEVICE_ERROR       (EFI_ERROR_BIT | 7)
#define EFI_WRITE_PROTECTED    (EFI_ERROR_BIT | 8)
#define EFI_OUT_OF_RESOURCES   (EFI_ERROR_BIT | 9)
#define EFI_NOT_FOUND          (EFI_ERROR_BIT | 14)
#define EFI_ACCESS_DENIED      (EFI_ERROR_BIT | 15)
#define EFI_TIMEOUT            (EFI_ERROR_BIT | 18)
#define EFI_NOT_STARTED        (EFI_ERROR_BIT | 23)
#define EFI_ABORTED            (EFI_ERROR_BIT | 21)
#define EFI_SECURITY_VIOLATION (EFI_ERROR_BIT | 26)

/* EFI_TABLE_HEADER, common to every UEFI table (System, Boot Services, Runtime Services).
 * §4.2.1. */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

#endif
