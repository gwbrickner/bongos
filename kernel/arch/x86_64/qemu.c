/* See kernel/include/arch/qemu.h. */
#include <arch/qemu.h>

#include "include/io-impl.h"

_Noreturn void archDebugExit(uint8_t code) {
    ioOutByte(0xF4, code);
    for (;;) {
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}
