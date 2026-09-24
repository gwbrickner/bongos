/* The UEFI loader entry point (ROADMAP M1.2). Prints the banner to ConOut and COM1, then halts.
 * The real flow -- boot.cfg, kernel/initrd loading, GOP, RSDP, BootInfo, ExitBootServices -- is
 * ARCHITECTURE §5.5 and lands in M1.3/M1.4. */
#include "branding.h"
#include "include/efi/efi.h"
#include "serial.h"

#define LOADER_BANNER_TEXT BRANDING_NAME " loader"
/* Printed only over raw COM1, never through ConOut: OVMF's ConOut is itself mirrored to the
 * serial log by TerminalDxe, so a serial log containing LOADER_BANNER_TEXT alone doesn't prove
 * loaderSerialWriteString() (the path that's all that's left after ExitBootServices, from M1.3
 * on) actually ran. The harness's --expect-serial checks for both (D-057, mk/test.mk). */
#define LOADER_COM1_MARKER "loader: com1 ok"

static void loaderHalt(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

EFI_STATUS EFIAPI efiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;

    /* UEFI §3.1.2: the firmware arms a 5-minute watchdog before starting a boot option. This
     * loader never calls ExitBootServices, so nothing else would ever disarm it. */
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    loaderSerialInit();
    loaderSerialWriteString(LOADER_BANNER_TEXT "\n");
    loaderSerialWriteString(LOADER_COM1_MARKER "\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut,
                                      (CHAR16 *)L"" LOADER_BANNER_TEXT L"\r\n");

    loaderHalt();
    return EFI_SUCCESS; /* unreachable: loaderHalt() never returns */
}
