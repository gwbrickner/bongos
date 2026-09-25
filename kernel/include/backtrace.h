/* Symbolized frame-pointer backtraces (ARCHITECTURE §24). Requires -fno-omit-frame-pointer
 * (kernel.mk always sets it) and every legitimate frame's rbp to point inside a known kernel
 * stack range (the boot stack or one of the three IST stacks, kernel/include/sections.h). */
#ifndef KERNEL_BACKTRACE_H
#define KERNEL_BACKTRACE_H

#include <stdint.h>

/* Walks the rbp chain starting at `fp`, filling `pcs` with up to `max` raw return addresses
 * (caller decides how/whether to symbolize them). Stops at a NULL/misaligned rbp, one that falls
 * outside every known stack range, or one that doesn't strictly increase within the stack range
 * it's currently walking (switching to a different known range mid-walk is allowed, e.g. an IST
 * handler's chain continuing into the boot stack). Returns the number of entries written. No
 * locks, IRQ-safe; pure (touches only `pcs`). */
uint32_t backtraceCapture(uint64_t fp, uint64_t *pcs, uint32_t max);

/* Prints a backtrace to serial (raw, via klogRaw -- no klog prefix), one "  #N 0x<pc> <sym>+0x
 * <off>/0x<size>" line per frame (or "  #N 0x<pc> ?" when symbolize() can't place it), up to 32
 * frames. Frame #0 is `faultPc` itself, looked up as-is. Every later frame's return address is
 * looked up at (return address - 1) -- a call to a _Noreturn function is the last instruction of
 * its caller-visible function, so the raw return address can land on the first byte of the next
 * symbol -- but the printed offset uses the real return address. No locks, IRQ-safe. */
void backtracePrint(uint64_t faultPc, uint64_t fp);

#endif
