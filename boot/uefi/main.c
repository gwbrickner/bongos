/* The UEFI loader entry point (ARCHITECTURE §5.5, ROADMAP M1.3). Prints the banner, then hands off
 * to handoff.c for the real flow: boot.cfg, kernel loading, page tables, BootInfo,
 * ExitBootServices, and the jump into the kernel. Only returns here on failure. */
#include "branding.h"
#include "handoff.h"
#include "include/efi/efi.h"
#include "serial.h"

#define LOADER_BANNER_TEXT BRANDING_NAME " loader"
/* Printed only over raw COM1, never through ConOut: OVMF's ConOut is itself mirrored to the
 * serial log by TerminalDxe, so a serial log containing LOADER_BANNER_TEXT alone doesn't prove
 * loaderSerialWriteString() (the only path left after ExitBootServices) actually ran. Kept for
 * debugging even though `make test` no longer asserts on it directly (D-057/D-063: M1.3 switched
 * to the real KTEST/isa-debug-exit protocol). */
#define LOADER_COM1_MARKER "loader: com1 ok"

static void loaderHalt(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static uint64_t loaderRdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

EFI_STATUS EFIAPI efiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    uint64_t loaderTsc =
        loaderRdtsc(); /* ARCHITECTURE §5.5 step 1: captured before anything else */

    /* UEFI §3.1.2: the firmware arms a 5-minute watchdog before starting a boot option. handoffRun
     * disarms nothing else before ExitBootServices removes the concept entirely, so clear it now.
     */
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    loaderSerialInit();
    loaderSerialWriteString(LOADER_BANNER_TEXT "\n");
    loaderSerialWriteString(LOADER_COM1_MARKER "\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut,
                                      (CHAR16 *)L"" LOADER_BANNER_TEXT L"\r\n");

    EFI_STATUS status = handoffRun(ImageHandle, SystemTable, loaderTsc);
    /* handoffRun() only returns on failure -- success ends in a jump to the kernel that never
     * comes back. Whether ConOut/BootServices are still usable depends on how far it got before
     * failing, so report over COM1 only (always safe) and let the harness/human read the log.
     * Design note (out of scope for this milestone): a pre-ExitBootServices failure could instead
     * `return status` here and let the firmware fall through to its next boot option, rather than
     * halting -- not done, since a silent fallback to another OS/entry could be more confusing
     * than a clear halt with a logged reason. */
    loaderSerialWriteString("loader: handoff failed, halting\n");
    (void)status;
    loaderHalt();
    return EFI_SUCCESS; /* unreachable */
}
