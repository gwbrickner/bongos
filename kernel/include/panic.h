/* Kernel panic (ARCHITECTURE §24). Prints a banner, a symbolized backtrace (M2.1, D-073), and
 * either reports a KTEST FAIL and exits (ktest= active) or halts. */
#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <arch/trap.h>

/* Disables interrupts, prints "PANIC: <message>", a symbolized backtrace starting at the
 * caller's own frame, then either reports a KTEST FAIL and exits through archDebugExit() (if
 * `ktest=` is active and a matching ktestExpectPanic() isn't armed) or halts forever. A panic
 * while already panicking prints one raw line and, past three nested panics, halts immediately
 * rather than recursing further. No locks (interrupts are disabled immediately), boot-time only;
 * never returns (unless a matching expected-panic recovers the caller via a non-local jump,
 * D-076 -- from the panicking call site's point of view this still never returns). */
_Noreturn void panic(const char *fmt, ...);

/* Same as panic(), but for a fatal CPU exception: prints the trap frame (vector, error code,
 * registers, decoded error code) via archTrapFrameDump() before the backtrace, and starts the
 * backtrace at the trap frame's own (RIP, RBP) rather than the caller's. Called only from
 * kernel/arch/x86_64/trap.c's archTrapDispatch(). No locks; never returns (barring an
 * expected-panic recovery, as above). */
_Noreturn void panicTrap(const TrapFrame *f, const char *fmt, ...);

#endif
