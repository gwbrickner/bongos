/* stage2's C entry (D-103/D-104): the 16-bit entry (entry.asm) already switched to protected
 * mode before calling this. Uses the same shared serial/CPU drivers the UEFI loader does
 * (boot/common/hw/serial.c, cpu.c) -- proving the i386 build of the shared loader code, not just
 * a one-off toolchain smoke test. _Noreturn: pm_entry's caller treats returning here as
 * unreachable. */
#include "bootmem.h"
#include "loader-cpu.h"
#include "loader-serial.h"
#include "rm.h"

#include <stdbool.h>

static void haltForever(void) {
    for (;;) {
        __asm__ volatile("cli\n\thlt");
    }
}

/* First real exercise of the thunk (D-102): INT 12h takes no input registers and returns the
 * conventional-memory size in KiB in AX -- about the simplest possible real BIOS call to prove
 * rmInt() actually works end to end (the far-call-through-the-IVT emulation, the PM<->RM<->PM
 * segment/stack juggling, and the register marshaling) before anything real (E820, disk, VBE)
 * depends on it. */
static void thunkSelfTest(void) {
    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    rmInt(0x12, &r);
    loaderSerialWriteString("loader: INT 12h reports ");
    loaderSerialWriteUint(r.eax & 0xFFFFu);
    loaderSerialWriteString(" KiB low memory\n");
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

    thunkSelfTest();

    haltForever();
}
