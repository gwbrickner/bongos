/* Kernel panic (ARCHITECTURE §24). M1.3: serial banner + raw frame-pointer backtrace only; the
 * framebuffer panic screen and symbolized backtrace arrive with M1.4/M2.1. */
#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

/* Disables interrupts, prints "PANIC: <message>" plus a raw return-address backtrace (up to 16
 * frames) to serial, then either reports a KTEST FAIL and exits through archDebugExit() (if
 * `ktest=` is active) or halts forever. A panic while already panicking halts immediately rather
 * than recursing. No locks (interrupts are disabled immediately), boot-time only; never returns. */
_Noreturn void panic(const char *fmt, ...);

#endif
