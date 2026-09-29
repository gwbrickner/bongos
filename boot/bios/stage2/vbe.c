#include "vbe.h"

#include "bootmem.h"
#include "bootvideo.h"
#include "loader-serial.h"
#include "rm.h"

#include <stdbool.h>

/* Fixed low-memory scratch offsets (docs/specs/bios-boot.md §3, D-101). */
#define VBE_INFO_ADDR      0x1200u /* VbeInfoBlock, 512 bytes */
#define VBE_MODE_INFO_ADDR 0x1400u /* ModeInfoBlock, 256 bytes */

#define VBE_OK_AX         0x004Fu /* AL=4Fh (function supported) + AH=00h (call succeeded) */
#define VBE_MODE_LIST_MAX 256u /* bounds the mode-list walk regardless of what the BIOS reports */
#define VBE_LFB_BIT       0x4000u /* Set Mode bit 14: use the linear framebuffer */
#define VBE_VERSION_3_0   0x0300u /* VbeVersion at/above which the LinBytesPerScanLine and
                                     Lin{Red,Green,Blue,Rsvd}{MaskSize,FieldPosition} fields
                                     (D-114) are valid to read */

/* VBE far pointers (VbeInfoBlock's VideoModePtr) pack a real-mode segment:offset into one u32,
 * offset in the low word -- the same layout rm.asm reads directly out of the IVT. Always resolves
 * to a linear address below 1 MiB, so it's readable straight through stage2's flat 32-bit PM
 * addressing (paging is off) without another thunk round trip. */
static uint32_t vbeFarPtrToLinear(uint32_t farPtr) {
    uint16_t seg = (uint16_t)(farPtr >> 16);
    uint16_t off = (uint16_t)(farPtr & 0xFFFFu);
    return ((uint32_t)seg << 4) + off;
}

static uint32_t vbeMaskFromSizePos(uint8_t size, uint8_t pos) {
    if (size == 0) {
        return 0;
    }
    return ((1u << size) - 1u) << pos;
}

/* INT 10h AX=4F00h (Get VBE Controller Info). "VBE2" pre-filled into the buffer requests the
 * VBE 2.0+ extended fields (in particular VideoModePtr's reach beyond the original 256-byte
 * block); real VBE 2.0+ BIOSes look for it and turn it into "VESA" plus the extended fields on
 * return. Also returns VbeVersion (offset 4, word) and TotalMemory (offset 0x12, word, in 64 KiB
 * blocks -- widened to bytes here) so the caller can gate the VBE 3.0-only ModeInfoBlock fields
 * and bound a mode's framebuffer size against what the card actually reports having (D-114). */
static bool vbeGetControllerInfo(uint32_t *outVideoModePtr, uint16_t *outVbeVersion,
                                 uint64_t *outTotalMemoryBytes) {
    uint8_t *buf = (uint8_t *)(uintptr_t)VBE_INFO_ADDR;
    bootMemset(buf, 0, 512);
    buf[0] = 'V';
    buf[1] = 'B';
    buf[2] = 'E';
    buf[3] = '2';

    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    r.eax = 0x4F00u;
    r.edi = VBE_INFO_ADDR;
    r.es = 0;
    rmInt(0x10, &r);
    if ((r.eax & 0xFFFFu) != VBE_OK_AX) {
        return false;
    }
    if (buf[0] != 'V' || buf[1] != 'E' || buf[2] != 'S' || buf[3] != 'A') {
        return false;
    }

    uint32_t videoModePtr = 0;
    bootMemcpy(&videoModePtr, buf + 14, sizeof(videoModePtr));
    *outVideoModePtr = videoModePtr;

    uint16_t vbeVersion = 0;
    bootMemcpy(&vbeVersion, buf + 4, sizeof(vbeVersion));
    *outVbeVersion = vbeVersion;

    uint16_t totalMemoryBlocks = 0;
    bootMemcpy(&totalMemoryBlocks, buf + 0x12, sizeof(totalMemoryBlocks));
    *outTotalMemoryBytes = (uint64_t)totalMemoryBlocks * 65536u;
    return true;
}

/* INT 10h AX=4F01h (Get Mode Info). Leaves the result in the fixed VBE_MODE_INFO_ADDR buffer --
 * not reentrant/not safe to interleave between two modes, same as every other thunk-backed
 * scratch area in this loader. */
static bool vbeGetModeInfo(uint16_t mode) {
    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    r.eax = 0x4F01u;
    r.ecx = mode;
    r.edi = VBE_MODE_INFO_ADDR;
    r.es = 0;
    rmInt(0x10, &r);
    return (r.eax & 0xFFFFu) == VBE_OK_AX;
}

/* Reduces the ModeInfoBlock vbeGetModeInfo() just left at VBE_MODE_INFO_ADDR to bootvideo.c's
 * firmware-agnostic BootVideoMode shape. Only 32bpp direct-color modes with a linear framebuffer
 * are considered acceptable here (bootVideoAccept() would reject anything else anyway, but
 * checking ModeAttributes/BitsPerPixel/MemoryModel directly avoids feeding it masks derived from
 * a banked or paletted mode's meaningless Red/Green/BlueMaskSize fields).
 *
 * `vbeVersion`/`totalMemoryBytes` come from the controller-info call (D-114): at VBE 3.0+, the
 * base BytesPerScanLine/Red-Green-Blue-RsvdMaskSize/FieldPosition fields describe the *banked*
 * mode, which can legitimately differ from the linear one this loader actually uses (the
 * Lin*-prefixed fields at offsets 50/54-61) -- reading the banked fields on such hardware could
 * pick a pitch or mask that doesn't match the linear framebuffer this loader writes through.
 * `totalMemoryBytes` bounds the framebuffer size against what the card actually reports having,
 * so a mode whose computed footprint doesn't fit can't be picked. */
static bool vbeModeInfoToVideoMode(uint16_t mode, uint16_t vbeVersion, uint64_t totalMemoryBytes,
                                   BootVideoMode *out) {
    const uint8_t *mi = (const uint8_t *)(uintptr_t)VBE_MODE_INFO_ADDR;

    uint16_t attrs = 0;
    bootMemcpy(&attrs, mi + 0, sizeof(attrs));
    if ((attrs & 0x01u) == 0 || (attrs & 0x10u) == 0 || (attrs & 0x80u) == 0) {
        return false; /* not supported by this hardware / not graphics / no linear framebuffer */
    }

    uint8_t bitsPerPixel = mi[25];
    uint8_t memoryModel = mi[27];
    if (bitsPerPixel != 32 || memoryModel != 6) {
        return false; /* 6 = direct color; only 32bpp direct color has a stable mask layout */
    }

    uint16_t bytesPerScanLine = 0, xRes = 0, yRes = 0;
    bootMemcpy(&bytesPerScanLine, mi + 16, sizeof(bytesPerScanLine));
    bootMemcpy(&xRes, mi + 18, sizeof(xRes));
    bootMemcpy(&yRes, mi + 20, sizeof(yRes));
    uint8_t redSize = mi[31], redPos = mi[32];
    uint8_t greenSize = mi[33], greenPos = mi[34];
    uint8_t blueSize = mi[35], bluePos = mi[36];
    uint8_t rsvdSize = mi[37], rsvdPos = mi[38];
    uint32_t physBase = 0;
    bootMemcpy(&physBase, mi + 40, sizeof(physBase));

    if (vbeVersion >= VBE_VERSION_3_0) {
        uint16_t linBytesPerScanLine = 0;
        bootMemcpy(&linBytesPerScanLine, mi + 50, sizeof(linBytesPerScanLine));
        if (linBytesPerScanLine != 0) {
            bytesPerScanLine = linBytesPerScanLine;
        }
        redSize = mi[54];
        redPos = mi[55];
        greenSize = mi[56];
        greenPos = mi[57];
        blueSize = mi[58];
        bluePos = mi[59];
        rsvdSize = mi[60];
        rsvdPos = mi[61];
    }

    bootMemset(out, 0, sizeof(*out));
    out->id = mode;
    out->width = xRes;
    out->height = yRes;
    out->pitch = bytesPerScanLine;
    out->redMask = vbeMaskFromSizePos(redSize, redPos);
    out->greenMask = vbeMaskFromSizePos(greenSize, greenPos);
    out->blueMask = vbeMaskFromSizePos(blueSize, bluePos);
    out->reservedMask = vbeMaskFromSizePos(rsvdSize, rsvdPos); /* bootVideoAccept() needs the
        combined mask's top bit in 24..31 to confirm a real 32-bit pixel (D-068/D-109) -- a
        24-bit RGB triple alone tops out at bit 23, and every real 32bpp VBE mode's reserved/
        alpha field is exactly what supplies the missing high bits */
    out->fbPhys = physBase;

    if (totalMemoryBytes != 0) {
        uint64_t fbBytes = (uint64_t)out->pitch * (uint64_t)out->height;
        if (fbBytes > totalMemoryBytes) {
            return false; /* doesn't fit the card's own reported memory size */
        }
    }
    return true;
}

/* Streams every VBE mode through bootvideo.c's picker (D-109), mirroring gop.c's gopPickMode().
 * Also hands back the controller's VbeVersion/TotalMemory (already fetched here) so the caller's
 * later re-query of the picked mode doesn't need a second AX=4F00h call. */
static bool vbePickMode(uint32_t resWidth, uint32_t resHeight, uint16_t *outMode,
                        uint16_t *outVbeVersion, uint64_t *outTotalMemoryBytes) {
    uint32_t videoModePtr = 0;
    if (!vbeGetControllerInfo(&videoModePtr, outVbeVersion, outTotalMemoryBytes)) {
        return false;
    }

    BootVideoPicker picker;
    bootVideoPickerInit(&picker, resWidth, resHeight);

    const uint8_t *modeList = (const uint8_t *)(uintptr_t)vbeFarPtrToLinear(videoModePtr);
    for (uint32_t i = 0; i < VBE_MODE_LIST_MAX; i++) {
        uint16_t mode = 0;
        bootMemcpy(&mode, modeList + i * 2u, sizeof(mode));
        if (mode == 0xFFFFu) {
            break;
        }
        if (!vbeGetModeInfo(mode)) {
            continue;
        }
        BootVideoMode candidate;
        if (!vbeModeInfoToVideoMode(mode, *outVbeVersion, *outTotalMemoryBytes, &candidate)) {
            continue;
        }
        if (bootVideoAccept(&candidate)) {
            bootVideoPickerOffer(&picker, &candidate);
        }
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
    *outMode = (uint16_t)picked.id;
    return true;
}

void vbeSetMode(uint32_t resWidth, uint32_t resHeight, BootFramebuffer *fb) {
    bootMemset(fb, 0, sizeof(*fb));

    uint16_t mode = 0, vbeVersion = 0;
    uint64_t totalMemoryBytes = 0;
    if (!vbePickMode(resWidth, resHeight, &mode, &vbeVersion, &totalMemoryBytes)) {
        return; /* no acceptable VBE mode; leave fb zeroed (D-064) */
    }

    /* Re-query the picked mode's info: the fixed VBE_MODE_INFO_ADDR buffer was overwritten by
     * every later mode vbePickMode() probed after it during enumeration. */
    if (!vbeGetModeInfo(mode)) {
        return;
    }
    BootVideoMode picked;
    if (!vbeModeInfoToVideoMode(mode, vbeVersion, totalMemoryBytes, &picked)) {
        loaderSerialWriteString("loader: VBE mode info changed on re-query; aborting\n");
        return;
    }
    if (!bootVideoAccept(&picked)) {
        /* Shouldn't happen (this same mode passed bootVideoAccept() during picking), but a BIOS
         * that reports a different ModeInfoBlock on this second query than it did while
         * enumerating is not something to trust blindly: bootVideoToFramebuffer() below computes
         * shift/size via __builtin_ctz/popcount on the masks, which is undefined for a zero mask.
         */
        loaderSerialWriteString("loader: VBE mode failed re-accept; aborting\n");
        return;
    }

    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    r.eax = 0x4F02u;
    r.ebx = (uint32_t)mode | VBE_LFB_BIT;
    rmInt(0x10, &r);
    if ((r.eax & 0xFFFFu) != VBE_OK_AX) {
        return; /* Set Mode failed; leave fb zeroed rather than fail the whole boot */
    }

    bootVideoToFramebuffer(&picked, fb);
}
