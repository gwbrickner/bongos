/* See file.h. */
#include "file.h"

#include "include/efi/guids.h"

EFI_STATUS loaderOpenVolume(EFI_SYSTEM_TABLE *st, EFI_HANDLE imageHandle,
                            EFI_FILE_PROTOCOL **outRoot) {
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage = NULL;
    EFI_STATUS status = st->BootServices->HandleProtocol(
        imageHandle, (EFI_GUID *)&gEfiLoadedImageProtocolGuid, (VOID **)&loadedImage);
    if (EFI_ERROR(status)) {
        return status;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    status = st->BootServices->HandleProtocol(
        loadedImage->DeviceHandle, (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, (VOID **)&sfs);
    if (EFI_ERROR(status)) {
        return status;
    }

    return sfs->OpenVolume(sfs, outRoot);
}

void loaderAsciiPathToWide(const char *asciiPath, CHAR16 *out, UINTN outCapChars) {
    UINTN i = 0;
    for (; asciiPath[i] != '\0' && i < outCapChars - 1; i++) {
        char c = asciiPath[i];
        out[i] = (CHAR16)((unsigned char)(c == '/' ? '\\' : c));
    }
    out[i] = 0;
}

EFI_STATUS loaderReadFile(EFI_SYSTEM_TABLE *st, EFI_FILE_PROTOCOL *root, const CHAR16 *path,
                          uint8_t **outBuf, uint64_t *outSize) {
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN infoSize = 0;
    status = file->GetInfo(file, (EFI_GUID *)&gEfiFileInfoGuid, &infoSize, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        file->Close(file);
        return EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;
    }
    VOID *infoBuf = NULL;
    status = st->BootServices->AllocatePool(EfiLoaderData, infoSize, &infoBuf);
    if (EFI_ERROR(status)) {
        file->Close(file);
        return status;
    }
    status = file->GetInfo(file, (EFI_GUID *)&gEfiFileInfoGuid, &infoSize, infoBuf);
    if (EFI_ERROR(status)) {
        st->BootServices->FreePool(infoBuf);
        file->Close(file);
        return status;
    }
    uint64_t fileSize = ((EFI_FILE_INFO *)infoBuf)->FileSize;
    st->BootServices->FreePool(infoBuf);

    /* AllocatePool(0) is undefined by the spec; a zero-byte file still needs a (unused) buffer
     * pointer for the caller's later FreePool(). */
    VOID *dataBuf = NULL;
    status = st->BootServices->AllocatePool(EfiLoaderData, fileSize > 0 ? fileSize : 1, &dataBuf);
    if (EFI_ERROR(status)) {
        file->Close(file);
        return status;
    }

    uint64_t totalRead = 0;
    while (totalRead < fileSize) {
        UINTN chunk = (UINTN)(fileSize - totalRead);
        status = file->Read(file, &chunk, (uint8_t *)dataBuf + totalRead);
        if (EFI_ERROR(status)) {
            st->BootServices->FreePool(dataBuf);
            file->Close(file);
            return status;
        }
        if (chunk == 0) {
            break;
        }
        totalRead += chunk;
    }
    file->Close(file);

    *outBuf = (uint8_t *)dataBuf;
    *outSize = totalRead;
    return EFI_SUCCESS;
}
