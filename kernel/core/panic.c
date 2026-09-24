/* See panic.h. */
#include "panic.h"

#include "format.h"
#include "klog.h"
#include "ktest.h"
#include "sections.h"

#include <arch/cpu.h>
#include <arch/qemu.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

static bool panicking = false;

_Noreturn void panic(const char *fmt, ...) {
    archDisableInterrupts();
    if (panicking) {
        archHaltForever();
    }
    panicking = true;

    char message[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    char banner[256];
    ksnprintf(banner, sizeof(banner), "\r\nPANIC: %s\n", message);
    klogRaw(banner);

    /* Raw frame-pointer backtrace: -fno-omit-frame-pointer keeps rbp chained through every
     * function's prologue, so this needs no symbol table (that arrives in M2.1). Stops at a NULL
     * rbp (kernelMain's own frame has none below it, since entry.asm zeroed rbp before calling
     * it) or once rbp strays outside the boot stack -- a corrupted chain must not walk into
     * unmapped or unrelated memory. */
    uint64_t rbp = archFramePointer();
    uint64_t stackBottom = (uint64_t)(uintptr_t)kernelBootStackBottom;
    uint64_t stackTop = (uint64_t)(uintptr_t)kernelBootStackTop;
    for (int frame = 0; frame < 16 && rbp != 0; frame++) {
        if (rbp < stackBottom || rbp >= stackTop) {
            break;
        }
        const uint64_t *frameWords = (const uint64_t *)(uintptr_t)rbp;
        uint64_t savedRbp = frameWords[0];
        uint64_t returnAddr = frameWords[1];

        char frameLine[64];
        ksnprintf(frameLine, sizeof(frameLine), "  #%d 0x%016llx\n", frame, returnAddr);
        klogRaw(frameLine);
        rbp = savedRbp;
    }

    if (ktestIsActive()) {
        const char *name = ktestCurrentName();
        char failLine[256];
        ksnprintf(failLine, sizeof(failLine), "KTEST FAIL %s: panic: %s\n",
                  name != NULL ? name : "kernel", message);
        klogRaw(failLine);
        archDebugExit(0x11);
    }

    archHaltForever();
}
