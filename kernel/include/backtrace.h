/* Frame-pointer backtrace, shared by panic() and the trap-report path (ARCHITECTURE §24). No
 * symbol table yet (that's the next M2.1 step, KSYM v1) -- prints raw return addresses. */
#ifndef KERNEL_BACKTRACE_H
#define KERNEL_BACKTRACE_H

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

#endif
