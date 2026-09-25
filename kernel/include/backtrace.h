/* Frame-pointer backtrace, shared by panic() and the trap-report path (ARCHITECTURE §24).
 * Symbolized via KSYM v1 (ksym.h, D-075). */
#ifndef KERNEL_BACKTRACE_H
#define KERNEL_BACKTRACE_H

#include <stddef.h>
#include <stdint.h>

/* Prints a backtrace to klogRaw, one "  #N 0x<addr>\n" line per frame, up to 32 frames. If `pc`
 * is nonzero, it's printed first as frame #0 (the trap path's own faulting RIP, which has no
 * return-address slot of its own to walk to); frame numbering then continues from `fp`'s chain
 * starting at #1 (or #0 if `pc` was 0, matching plain panic()'s use with no separate PC).
 * Bounds-checks every frame pointer against the four statically-known kernel stacks (the boot
 * stack and the three IST stacks, ARCHITECTURE §7.1) -- a frame pointer outside all of them, or
 * not 8-aligned, or that doesn't strictly increase while it stays within the *same* stack (an
 * IST-vectored fault's chain can legitimately cross from an IST stack into the boot stack, since
 * RBP isn't switched by the IST mechanism, only RSP is), stops the walk rather than risk reading
 * unmapped or unrelated memory. No locks; safe to call from any trap context (single-threaded,
 * IF=0 throughout M2.1). */
void backtracePrint(uint64_t pc, uint64_t fp);

/* Same walk as backtracePrint(), but writes each frame's return address into `out` (up to `max`
 * entries) instead of printing, and returns how many it wrote -- for ktests to check the walk's
 * correctness programmatically instead of scraping klog text. No locks; safe from any context. */
size_t backtraceCapture(uint64_t fp, uint64_t *out, size_t max);

#endif
