/* stage2's C entry (D-103/D-104): the 16-bit entry (entry.asm) already switched to protected
 * mode before calling this. Uses the same shared serial/CPU drivers the UEFI loader does
 * (boot/common/hw/serial.c, cpu.c) -- proving the i386 build of the shared loader code, not just
 * a one-off toolchain smoke test. Only prints the banner; handoff.c has the real flow (kernel
 * load, page tables, BootInfo, the jump into long mode), mirroring boot/uefi/main.c's own thin
 * entry point. */
#include "handoff.h"
#include "loader-serial.h"

void stage2Main(void) {
    loaderSerialInit();
    loaderSerialWriteString("loader: stage2 c environment\n");

    handoffRun(); /* _Noreturn: only returns by halting on a fatal error */
}
