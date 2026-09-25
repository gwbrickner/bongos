/* See panic.h. */
#include "panic.h"

#include "backtrace.h"
#include "format.h"
#include "klog.h"
#include "ktest.h"

#include <arch/cpu.h>
#include <arch/qemu.h>
#include <arch/trap.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

static int panicDepth = 0;

static _Noreturn void panicExit(const char *message) {
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

static _Noreturn void panicV(const TrapFrame *frame, uint64_t callerFp, const char *fmt,
                             va_list ap) {
    archDisableInterrupts();
    panicDepth++;

    char message[200];
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    if (panicDepth >= 3) {
        archHaltForever();
    }
    if (panicDepth == 2) {
        klogRaw("\r\nPANIC while already panicking; halting\n");
        panicExit(message);
    }

    char banner[256];
    ksnprintf(banner, sizeof(banner), "\r\nPANIC: %s\n", message);
    klogRaw(banner);

    if (frame != NULL) {
        archTrapFrameDump(frame);
        backtracePrint(archTrapFramePc(frame), archTrapFrameFp(frame));
    } else {
        backtracePrint(0, callerFp);
    }

    /* A ktest deliberately provoking this exact panic recovers instead of the fatal path below --
     * checked only after the full report above prints, so a recovered panic still leaves the same
     * symbolized banner/backtrace in the log that a real one would (ARCHITECTURE §24's "panic
     * output includes a symbolized backtrace" guarantee doesn't stop applying just because a
     * ktest was the one that triggered it). */
    if (ktestPanicExpected(message)) {
        klogRaw("ktest: expected panic caught\n");
        panicDepth = 0;
        ktestPanicRecover();
    }

    panicExit(message);
}

_Noreturn void panic(const char *fmt, ...) {
    uint64_t callerFp = archFramePointer(); /* always_inline: this is panic()'s own rbp, whose
                                             * frame record points at panic()'s *caller* */
    va_list ap;
    va_start(ap, fmt);
    panicV(NULL, callerFp, fmt, ap);
}

_Noreturn void panicTrap(const TrapFrame *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    panicV(f, 0, fmt, ap);
}
