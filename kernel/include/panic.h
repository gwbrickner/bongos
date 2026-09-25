/* Kernel panic (ARCHITECTURE §24). The framebuffer panic screen (M1.4) mirrors every klog line,
 * including panic's own, to the screen; M2.1 added the symbolized backtrace (KSYM v1, D-075). */
#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <stdbool.h>

/* Disables interrupts, prints "PANIC: <message>" plus a symbolized return-address backtrace (up
 * to 32 frames, kernel/core/backtrace.c's BACKTRACE_MAX_FRAMES) to serial, then either reports a
 * KTEST FAIL and exits through archDebugExit() (if `ktest=` is active) or halts forever. A panic
 * while already panicking halts immediately rather than recursing. No locks (interrupts are
 * disabled immediately), boot-time only; never returns. */
_Noreturn void panic(const char *fmt, ...);

/* The three pieces panic() is built from, exposed so trap.c's fault report can print its own
 * detailed header (vector, error code, CR2, registers, backtrace) in the right order -- between
 * the "PANIC: ..." banner and the ktest-exit/halt tail -- without duplicating panic()'s own
 * (unrelated) backtrace or racing its recursion guard.
 *
 * panicEnter() disables interrupts and returns true once, for the first caller; a second caller
 * (a panic during a panic) gets false back and must call panicNested() instead of proceeding.
 * panicFinish(message) is the shared tail: a KTEST FAIL line + archDebugExit() if `ktest=` is
 * active, otherwise halt forever. Neither returns except panicEnter(). No locks; boot-time only. */
bool panicEnter(void);
_Noreturn void panicNested(void);
_Noreturn void panicFinish(const char *message);

#endif
