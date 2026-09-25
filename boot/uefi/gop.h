/* GOP (Graphics Output Protocol) instance discovery and mode selection (ARCHITECTURE §5.5,
 * D-068). */
#ifndef LOADER_GOP_H
#define LOADER_GOP_H

#include "bootinfo.h"
#include "include/efi/efi.h"
#include "include/efi/graphics-output.h"

typedef struct {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop; /* NULL if no usable GOP instance was found at all */
} LoaderGop;

/* Finds a usable GOP instance: `st->ConsoleOutHandle`'s own GOP if it has one with a linear
 * framebuffer (`FrameBufferBase != 0`, current mode not PixelBltOnly), else the first
 * `LocateHandleBuffer(ByProtocol, GOP)` result meeting the same test (a machine with both a
 * discrete GPU and an iGPU can expose more than one). Logs the choice over COM1. Leaves
 * `lg->gop == NULL` (not an error) if nothing usable exists -- the caller treats "no
 * framebuffer" as valid, matching BootInfo v1's "fb.phys == 0 means not provided" (D-064). */
void loaderGopFind(EFI_SYSTEM_TABLE *st, LoaderGop *lg);

/* Picks a mode by D-068's rule (`resWidth`/`resHeight` both 0 means `auto`: largest area with
 * width <=3840 and height <=2160, ties to wider then lower mode number; otherwise an exact
 * WIDTHxHEIGHT match, falling back to auto with a logged warning if none exists) and calls
 * SetMode only if it differs from the current mode. Re-reads Mode->Info/FrameBufferBase
 * afterward (both can change across SetMode) and fills `fb` (BootFramebuffer.pitch is bytes,
 * ARCHITECTURE §5.3; bpp is always 32). Returns an error only for an unexpected firmware failure
 * (QueryMode/SetMode); "no acceptable mode exists at all" zeroes `*fb` and returns EFI_SUCCESS,
 * same "not provided" convention as `loaderGopFind`. */
EFI_STATUS loaderGopSetMode(EFI_SYSTEM_TABLE *st, LoaderGop *lg, uint32_t resWidth,
                            uint32_t resHeight, BootFramebuffer *fb);

#endif
