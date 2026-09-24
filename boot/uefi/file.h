/* Reading files off the ESP the loader itself came from (ARCHITECTURE §5.5 steps 3-4). */
#ifndef LOADER_FILE_H
#define LOADER_FILE_H

#include "include/efi/efi.h"

/* Opens the root directory of the volume `imageHandle` (the running loader) was loaded from:
 * LoadedImage -> DeviceHandle -> SimpleFileSystem -> OpenVolume. */
EFI_STATUS loaderOpenVolume(EFI_SYSTEM_TABLE *st, EFI_HANDLE imageHandle,
                            EFI_FILE_PROTOCOL **outRoot);

/* Converts an ASCII, '/'-separated absolute path (as boot.cfg uses, ARCHITECTURE §5.2) into a
 * CHR16, '\\'-separated one EFI_FILE_PROTOCOL::Open expects, truncating (never overflowing) at
 * `outCapChars` CHAR16s including the NUL. */
void loaderAsciiPathToWide(const char *asciiPath, CHAR16 *out, UINTN outCapChars);

/* Opens `path` under `root`, reads the whole file into a freshly EFI_LoaderData-AllocatePool'd
 * buffer (the two-call EFI_FILE_GET_INFO pattern for the size, then EFI_FILE_READ in a loop until
 * it returns 0 bytes, ARCHITECTURE §5.5), and returns it plus its size. The caller FreePool()s
 * `*outBuf` once done with it. */
EFI_STATUS loaderReadFile(EFI_SYSTEM_TABLE *st, EFI_FILE_PROTOCOL *root, const CHAR16 *path,
                          uint8_t **outBuf, uint64_t *outSize);

#endif
