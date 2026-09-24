/* The UEFI loader entry point (ROADMAP M1.2). Prints the banner to ConOut and COM1, then halts.
 * The real flow -- boot.cfg, kernel/initrd loading, GOP, RSDP, BootInfo, ExitBootServices -- is
 * ARCHITECTURE §5.5 and lands in M1.3/M1.4. */
#include "include/efi/efi.h"
#include "serial.h"

#define LOADER_BANNER "bongOS loader\n"

static void loaderHalt(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

EFI_STATUS EFIAPI efiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;

    loaderSerialInit();
    loaderSerialWriteString(LOADER_BANNER);

    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16 *)L"" LOADER_BANNER);

    loaderHalt();
    return EFI_SUCCESS; /* unreachable: loaderHalt() never returns */
}
