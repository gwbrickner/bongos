/* See panic.h. */
#include "panic.h"

#include "backtrace.h"
#include "format.h"
#include "klog.h"
#include "ktest.h"

#include <arch/cpu.h>
#include <arch/qemu.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

static bool panicking = false;

bool panicEnter(void) {
    archDisableInterrupts();
    if (panicking) {
        return false;
    }
    panicking = true;
    return true;
}

_Noreturn void panicNested(void) {
    klogRaw("PANIC while already panicking -- halting\n");
    if (ktestIsActive()) {
        archDebugExit(0x11);
    }
    archHaltForever();
}

_Noreturn void panicFinish(const char *message) {
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

_Noreturn void panic(const char *fmt, ...) {
    if (!panicEnter()) {
        panicNested();
    }

    char message[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    char banner[256];
    ksnprintf(banner, sizeof(banner), "\r\nPANIC: %s\n", message);
    klogRaw(banner);

    /* -fno-omit-frame-pointer keeps rbp chained through every function's prologue, so this needs
     * no symbol table yet (that's the very next M2.1 step, KSYM v1) -- backtracePrint() stops at
     * a NULL rbp (kernelMain's own frame has none below it, since entry.asm zeroed rbp before
     * calling it) or once rbp strays outside every known kernel stack. */
    backtracePrint(0, archFramePointer());

    panicFinish(message);
}
