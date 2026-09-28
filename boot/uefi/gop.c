/* See gop.h. GOP discovery, firmware-specific mode enumeration (ARCHITECTURE §5.5, D-068). The
 * accept/pick/framebuffer-fill rule itself is shared with the BIOS loader's VBE pick
 * (boot/common/bootvideo.c, D-109). */
#include "gop.h"

#include "bootmem.h"
#include "bootvideo.h"
#include "include/efi/guids.h"
#include "loader-serial.h"

static bool gopUsable(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    return gop != NULL && gop->Mode != NULL && gop->Mode->Info != NULL &&
           gop->Mode->Info->PixelFormat != PixelBltOnly && gop->Mode->FrameBufferBase != 0;
}

void loaderGopFind(EFI_SYSTEM_TABLE *st, LoaderGop *lg) {
    lg->gop = NULL;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (st->ConsoleOutHandle != NULL &&
        !EFI_ERROR(bs->HandleProtocol(
            st->ConsoleOutHandle, (EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, (VOID **)&gop)) &&
        gopUsable(gop)) {
        lg->gop = gop;
        loaderSerialWriteString("loader: GOP found on ConsoleOutHandle\n");
        return;
    }

    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = bs->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, NULL, &count, &handles);
    if (EFI_ERROR(status)) {
        loaderSerialWriteString("loader: no GOP handles found\n");
        return;
    }
    for (UINTN i = 0; i < count; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *candidate = NULL;
        if (!EFI_ERROR(bs->HandleProtocol(handles[i], (EFI_GUID *)&gEfiGraphicsOutputProtocolGuid,
                                          (VOID **)&candidate)) &&
            gopUsable(candidate)) {
            lg->gop = candidate;
            loaderSerialWriteString("loader: GOP found at handle index ");
            loaderSerialWriteUint((uint32_t)i);
            loaderSerialWriteString("\n");
            break;
        }
    }
    bs->FreePool(handles);
    if (lg->gop == NULL) {
        loaderSerialWriteString("loader: no usable GOP (linear framebuffer) found\n");
    }
}

/* Reduces one EFI_GRAPHICS_OUTPUT_MODE_INFORMATION to the firmware-agnostic BootVideoMode shape
 * bootvideo.c's accept/pick rule takes: RGBX/BGRX become explicit masks (D-109), PixelBitMask
 * carries its masks through as-is. `fbPhys`/`pitch` (in bytes) are filled from `gop`/`info`
 * directly since those aren't part of EFI_GRAPHICS_OUTPUT_MODE_INFORMATION itself. */
static void gopToVideoMode(const EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 modeNum,
                           const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, BootVideoMode *out) {
    bootMemset(out, 0, sizeof(*out));
    out->id = modeNum;
    out->width = info->HorizontalResolution;
    out->height = info->VerticalResolution;
    out->pitch = info->PixelsPerScanLine * 4u;
    out->fbPhys = gop->Mode->FrameBufferBase; /* only meaningful once this mode is actually set */
    switch (info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
            out->redMask = 0xFF;
            out->greenMask = 0xFF00;
            out->blueMask = 0xFF0000;
            out->reservedMask = 0xFF000000;
            break;
        case PixelBlueGreenRedReserved8BitPerColor:
            out->blueMask = 0xFF;
            out->greenMask = 0xFF00;
            out->redMask = 0xFF0000;
            out->reservedMask = 0xFF000000;
            break;
        case PixelBitMask:
            out->redMask = info->PixelInformation.RedMask;
            out->greenMask = info->PixelInformation.GreenMask;
            out->blueMask = info->PixelInformation.BlueMask;
            out->reservedMask = info->PixelInformation.ReservedMask;
            break;
        default:
            break; /* leave masks zero: bootVideoAccept() rejects PixelBltOnly and anything else */
    }
}

/* UEFI firmware quirk some implementations exhibit: QueryMode on a GOP instance that's never had
 * SetMode called on it yet returns EFI_NOT_STARTED. One SetMode(0) primes it. */
static EFI_STATUS gopQueryMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 mode, UINTN *sizeOfInfo,
                               EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info) {
    EFI_STATUS status = gop->QueryMode(gop, mode, sizeOfInfo, info);
    if (status == EFI_NOT_STARTED) {
        status = gop->SetMode(gop, 0);
        if (EFI_ERROR(status)) {
            return status;
        }
        status = gop->QueryMode(gop, mode, sizeOfInfo, info);
    }
    return status;
}

/* Streams every GOP mode through bootvideo.c's picker (D-109). Logs and retries as `auto` if an
 * explicit resolution was requested but not found. */
static bool gopPickMode(EFI_SYSTEM_TABLE *st, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t resWidth,
                        uint32_t resHeight, UINT32 *outMode) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    BootVideoPicker picker;
    bootVideoPickerInit(&picker, resWidth, resHeight);

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN sizeOfInfo = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        EFI_STATUS status = gopQueryMode(gop, m, &sizeOfInfo, &info);
        if (EFI_ERROR(status) || info == NULL) {
            continue;
        }
        BootVideoMode candidate;
        gopToVideoMode(gop, m, info, &candidate);
        if (bootVideoAccept(&candidate)) {
            bootVideoPickerOffer(&picker, &candidate);
        }
        bs->FreePool(info);
    }

    BootVideoMode picked;
    bool exactFellBack = false;
    if (!bootVideoPickerResult(&picker, &picked, &exactFellBack)) {
        return false;
    }
    if (exactFellBack) {
        loaderSerialWriteString("loader: resolution ");
        loaderSerialWriteUint(resWidth);
        loaderSerialWriteString("x");
        loaderSerialWriteUint(resHeight);
        loaderSerialWriteString(" unavailable, using auto\n");
    }
    *outMode = picked.id;
    return true;
}

EFI_STATUS loaderGopSetMode(EFI_SYSTEM_TABLE *st, LoaderGop *lg, uint32_t resWidth,
                            uint32_t resHeight, BootFramebuffer *fb) {
    bootMemset(fb, 0, sizeof(*fb));
    if (lg->gop == NULL) {
        return EFI_SUCCESS; /* no framebuffer available; not an error (D-064) */
    }
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = lg->gop;

    UINT32 mode = 0;
    if (!gopPickMode(st, gop, resWidth, resHeight, &mode)) {
        loaderSerialWriteString("loader: no acceptable GOP mode found\n");
        return EFI_SUCCESS;
    }

    if (mode != gop->Mode->Mode) {
        EFI_STATUS status = gop->SetMode(gop, mode);
        if (EFI_ERROR(status)) {
            loaderSerialWriteString("loader: GOP SetMode failed\n");
            return status;
        }
    }
    /* Mode->Info and FrameBufferBase can both change across SetMode -- SetMode() refreshes
     * `gop->Mode` itself, so read the live struct rather than re-querying. */
    BootVideoMode picked;
    gopToVideoMode(gop, mode, gop->Mode->Info, &picked);
    uint64_t size = (uint64_t)picked.pitch * (uint64_t)picked.height;
    if (gop->Mode->FrameBufferSize != 0 && size > gop->Mode->FrameBufferSize) {
        return EFI_SUCCESS; /* leave fb zeroed: doesn't fit its own reported size */
    }
    bootVideoToFramebuffer(&picked, fb);
    return EFI_SUCCESS;
}
