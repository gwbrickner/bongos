/* VBE mode pick + set (D-109, docs/specs/bios-boot.md §3): the BIOS-side half of the shared
 * accept/pick/framebuffer-fill rule in boot/common/bootvideo.c, whose UEFI-side half is
 * boot/uefi/gop.c. Only how each firmware enumerates/sets modes differs. */
#ifndef BOOT_BIOS_STAGE2_VBE_H
#define BOOT_BIOS_STAGE2_VBE_H

#include "bootinfo.h"

#include <stdint.h>

/* Enumerates VBE modes (INT 10h AX=4F00h/4F01h via the thunk), picks one through
 * bootvideo.c's shared rule (an exact `resWidth`x`resHeight` match if both are non-zero and
 * available, else the largest acceptable mode), and sets it (AX=4F02h, with the linear-
 * framebuffer bit set). Only 32bpp direct-color modes with a linear framebuffer are ever
 * considered (bootVideoAccept()'s own contract, D-068/D-109). Fills `*fb` from the result; if no
 * acceptable mode exists or the BIOS refuses to set the one picked, `*fb` is left zeroed --
 * having no framebuffer is not an error here (D-064), same as the UEFI loader's own
 * `loaderGopSetMode`. No locks, boot-time only; not reentrant (shares rmInt()'s scratch area). */
void vbeSetMode(uint32_t resWidth, uint32_t resHeight, BootFramebuffer *fb);

#endif
