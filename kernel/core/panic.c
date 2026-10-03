/* See panic.h. */
#include "panic.h"

#include "backtrace.h"
#include "format.h"
#include "atomic.h"
#include "klog.h"
#include "ktest.h"
#include "smp.h"

#include <arch/cpu.h>
#include <arch/qemu.h>
#include <arch/trap.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

static bool panicking = false;
static uint32_t panicCpu = UINT32_MAX; /* the CPU that won the race to panic */

/* D-197: the first CPU to get here owns the panic: it stops every other CPU (so their output cannot
 * interleave with the report) and prints. A CPU that loses the race to a panic on another CPU
 * parks silently; only a CPU that panics again while already panicking reports the nesting. */
bool panicEnter(void) {
    archDisableInterrupts();
    uint32_t me = smpThisCpu();
    if (ATOMIC_XCHG(&panicking, true, MEM_SEQ_CST)) { /* atomic: two CPUs must not both enter */
        if (ATOMIC_LOAD(&panicCpu, MEM_ACQUIRE) != me) {
            smpParkSelf();
        }
        return false;
    }
    ATOMIC_STORE(&panicCpu, me, MEM_RELEASE);
    smpStopOthers();
    return true;
}

bool panicInProgress(void) {
    return ATOMIC_LOAD(&panicking, MEM_RELAXED);
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

    /* -fno-omit-frame-pointer keeps rbp chained through every function's prologue; backtracePrint()
     * symbolizes each frame against the kernel's embedded KSYM v1 blob (D-075) and stops at a NULL
     * rbp (kernelMain's own frame has none below it, since entry.asm zeroed rbp before calling it)
     * or once rbp strays outside every known kernel stack. */
    backtracePrint(0, archFramePointer());

    panicFinish(message);
}

__attribute__((noinline)) _Noreturn void panicBug(const char *fmt, ...) {
    char message[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    archTrapCatchSoftware(TRAP_CATCH_KERNEL_BUG, (uint64_t)(uintptr_t)__builtin_return_address(0));

    if (!panicEnter()) {
        panicNested();
    }
    char banner[256];
    ksnprintf(banner, sizeof(banner), "\r\nPANIC: %s\n", message);
    klogRaw(banner);
    backtracePrint(0, archFramePointer());
    panicFinish(message);
}
