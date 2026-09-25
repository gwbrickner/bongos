/* See gop.h. GOP discovery and mode selection (ARCHITECTURE §5.5, D-068). */
#include "gop.h"

#include "bootmem.h"
#include "include/efi/guids.h"
#include "serial.h"

#define GOP_MAX_DIM   3840u
#define GOP_MAX_DIM_H 2160u

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

/* `mask` shifted so its lowest set bit sits at bit 0 is a contiguous run of 1s iff
 * `shifted & (shifted + 1) == 0` (e.g. 0b0111 + 1 = 0b1000, AND is 0; 0b0101 + 1 = 0b0110, AND is
 * nonzero). Requires mask != 0. */
static bool maskIsContiguous(uint32_t mask) {
    uint32_t shifted = mask >> __builtin_ctz(mask);
    return (shifted & (shifted + 1)) == 0;
}

/* D-068: accept RGBX/BGRX outright; accept a BitMask format only if R/G/B are each non-zero,
 * contiguous, and together span bits 24-31 (a real 32-bit pixel, not e.g. a 16-bit 565 mode). */
static bool gopAcceptMode(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info) {
    if (info->PixelsPerScanLine < info->HorizontalResolution) {
        return false;
    }
    switch (info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
        case PixelBlueGreenRedReserved8BitPerColor:
            return true;
        case PixelBitMask: {
            uint32_t r = info->PixelInformation.RedMask;
            uint32_t g = info->PixelInformation.GreenMask;
            uint32_t b = info->PixelInformation.BlueMask;
            uint32_t resv = info->PixelInformation.ReservedMask;
            if (r == 0 || g == 0 || b == 0) {
                return false;
            }
            if (!maskIsContiguous(r) || !maskIsContiguous(g) || !maskIsContiguous(b)) {
                return false;
            }
            uint32_t highest = r | g | b | resv;
            int top = 31 - __builtin_clz(highest);
            return top >= 24 && top <= 31;
        }
        default:
            return false;
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

/* Picks a mode number per D-068's rule; -1 (via *found = false) if nothing acceptable exists. */
static void gopPickMode(EFI_SYSTEM_TABLE *st, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t resWidth,
                        uint32_t resHeight, UINT32 *outMode, bool *found) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    *found = false;
    bool exact = resWidth != 0 && resHeight != 0;
    uint64_t bestArea = 0;
    uint32_t bestWidth = 0;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN sizeOfInfo = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        EFI_STATUS status = gopQueryMode(gop, m, &sizeOfInfo, &info);
        if (EFI_ERROR(status) || info == NULL) {
            continue;
        }
        if (!gopAcceptMode(info)) {
            bs->FreePool(info);
            continue;
        }
        if (exact) {
            if (info->HorizontalResolution == resWidth && info->VerticalResolution == resHeight) {
                *outMode = m;
                *found = true;
                bs->FreePool(info);
                break; /* lowest mode number wins: the loop is already ascending */
            }
        } else {
            if (info->HorizontalResolution <= GOP_MAX_DIM &&
                info->VerticalResolution <= GOP_MAX_DIM_H) {
                uint64_t area =
                    (uint64_t)info->HorizontalResolution * (uint64_t)info->VerticalResolution;
                if (area > bestArea ||
                    (area == bestArea && info->HorizontalResolution > bestWidth)) {
                    bestArea = area;
                    bestWidth = info->HorizontalResolution;
                    *outMode = m;
                    *found = true;
                }
            }
        }
        bs->FreePool(info);
    }

    if (exact && !*found) {
        loaderSerialWriteString("loader: resolution ");
        loaderSerialWriteUint(resWidth);
        loaderSerialWriteString("x");
        loaderSerialWriteUint(resHeight);
        loaderSerialWriteString(" unavailable, using auto\n");
        gopPickMode(st, gop, 0, 0, outMode, found);
    }
}

static void gopFillFramebuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, BootFramebuffer *fb) {
    bootMemset(fb, 0, sizeof(*fb));
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    uint64_t pitch = (uint64_t)info->PixelsPerScanLine * 4u;
    uint64_t size = pitch * (uint64_t)info->VerticalResolution;
    if (gop->Mode->FrameBufferSize != 0 && size > gop->Mode->FrameBufferSize) {
        return; /* leave fb zeroed: the mode's own geometry doesn't fit its reported size */
    }

    fb->phys = gop->Mode->FrameBufferBase;
    fb->width = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch = (uint32_t)pitch;
    fb->bpp = 32;
    switch (info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
            fb->redShift = 0;
            fb->greenShift = 8;
            fb->blueShift = 16;
            fb->redSize = fb->greenSize = fb->blueSize = 8;
            break;
        case PixelBlueGreenRedReserved8BitPerColor:
            fb->blueShift = 0;
            fb->greenShift = 8;
            fb->redShift = 16;
            fb->redSize = fb->greenSize = fb->blueSize = 8;
            break;
        case PixelBitMask: {
            uint32_t r = info->PixelInformation.RedMask;
            uint32_t g = info->PixelInformation.GreenMask;
            uint32_t b = info->PixelInformation.BlueMask;
            fb->redShift = (uint8_t)__builtin_ctz(r);
            fb->redSize = (uint8_t)__builtin_popcount(r);
            fb->greenShift = (uint8_t)__builtin_ctz(g);
            fb->greenSize = (uint8_t)__builtin_popcount(g);
            fb->blueShift = (uint8_t)__builtin_ctz(b);
            fb->blueSize = (uint8_t)__builtin_popcount(b);
            break;
        }
        default:
            bootMemset(fb, 0, sizeof(*fb));
            return;
    }
    if (fb->redSize > 8 || fb->greenSize > 8 || fb->blueSize > 8) {
        bootMemset(fb, 0, sizeof(*fb)); /* gopAcceptMode should already exclude this; defensive */
    }
}

EFI_STATUS loaderGopSetMode(EFI_SYSTEM_TABLE *st, LoaderGop *lg, uint32_t resWidth,
                            uint32_t resHeight, BootFramebuffer *fb) {
    bootMemset(fb, 0, sizeof(*fb));
    if (lg->gop == NULL) {
        return EFI_SUCCESS; /* no framebuffer available; not an error (D-064) */
    }
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = lg->gop;

    UINT32 mode = 0;
    bool found = false;
    gopPickMode(st, gop, resWidth, resHeight, &mode, &found);
    if (!found) {
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
    /* Mode->Info and FrameBufferBase can both change across SetMode -- read them fresh. */
    gopFillFramebuffer(gop, fb);
    return EFI_SUCCESS;
}
