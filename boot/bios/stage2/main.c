/* stage2's C entry (D-103/D-104): the 16-bit entry (entry.asm) already switched to protected
 * mode before calling this. Uses the same shared serial/CPU drivers the UEFI loader does
 * (boot/common/hw/serial.c, cpu.c) -- proving the i386 build of the shared loader code, not just
 * a one-off toolchain smoke test. _Noreturn: pm_entry's caller treats returning here as
 * unreachable. */
#include "loader-cpu.h"
#include "loader-serial.h"

#include <stdbool.h>

static void haltForever(void) {
    for (;;) {
        __asm__ volatile("cli\n\thlt");
    }
}

void stage2Main(void) {
    loaderSerialInit();
    loaderSerialWriteString("loader: stage2 c environment\n");

    bool has1G = false;
    if (!loaderCpuCheckLongModeFeatures(&has1G)) {
        loaderSerialWriteString("loader: CPU lacks NX; refusing to boot\n");
        haltForever();
    }
    loaderSerialWriteString("loader: CPU has NX");
    loaderSerialWriteString(has1G ? " and PDPE1GB\n" : "\n");

    haltForever();
}
