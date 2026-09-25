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

/* For a detected kernel-internal invariant violation (D-082: the pmm's misuse checks, e.g. a
 * double free) rather than a CPU-reported fault: offers the bug to a ktest-armed
 * archTrapCatch(TRAP_CATCH_KERNEL_BUG, ...) first (same pattern as __stack_chk_fail() and the
 * UBSan handlers) -- if one is armed and claims it, execution redirects back to that ktest's call
 * site and this never returns; otherwise it formats `fmt` and panics as usual. Caller contract:
 * no lock held and no shared state left half-mutated, since a claimed catch resumes by longjmp.
 * No locks; never returns. */
_Noreturn __attribute__((noinline)) void panicBug(const char *fmt, ...);

#endif
