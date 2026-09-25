/* Portable-facing declarations for exception dispatch (ARCHITECTURE §7.2/§23, D-074/D-078). The
 * implementation is entirely x86-specific and lives in kernel/arch/x86_64/trap.c.
 */
#ifndef KERNEL_INCLUDE_ARCH_TRAP_H
#define KERNEL_INCLUDE_ARCH_TRAP_H

#include <stdbool.h>
#include <stdint.h>

/* How many times #BP (int3) has fired and resumed since boot. No locks. */
uint64_t archBreakpointHits(void);

/* What archTrapCatch() actually caught: a CPU vector (`kind` is one of the TRAP_CATCH_VEC(v)
 * bits, and vector/errorCode/cr2 are the fault's own), or a software-raised kind
 * (TRAP_CATCH_STACK_SMASH/TRAP_CATCH_UBSAN, where only `rip` is meaningful). */
typedef struct {
    uint64_t kind;
    uint64_t vector;
    uint64_t errorCode;
    uint64_t cr2;
    uint64_t rip;
} TrapCatchInfo;

#define TRAP_CATCH_VEC(v)      (1ULL << (v)) /* v < 32: a raw CPU exception vector */
#define TRAP_CATCH_STACK_SMASH (1ULL << 32)  /* __stack_chk_fail() */
#define TRAP_CATCH_UBSAN       (1ULL << 33)  /* any __ubsan_handle_* trip */

/* Runs `fn(arg)` with `mask` armed (a bitmask of TRAP_CATCH_VEC(v)/TRAP_CATCH_STACK_SMASH/
 * TRAP_CATCH_UBSAN): if a matching fault/trip happens before `fn` returns, execution is redirected
 * back here instead of panicking, `*out` (if non-NULL) is filled with what was caught, and this
 * returns true. Returns false if `fn` ran to completion with nothing caught. ktest-only (panics if
 * called outside a ktest run), one-shot, non-nesting (panics if already armed), and can never
 * catch NMI/#DF/#MC (D-074 keeps those always-fatal; panics if `mask` includes one of them). No
 * locks; not reentrant. */
bool archTrapCatch(uint64_t mask, void (*fn)(void *), void *arg, TrapCatchInfo *out);

/* For __stack_chk_fail()/the UBSan handlers only: if a catch is armed and its mask includes
 * `kind` (TRAP_CATCH_STACK_SMASH or TRAP_CATCH_UBSAN), redirects execution back to the matching
 * archTrapCatch() call (never returns to the caller) and fills its `out`. Otherwise returns false,
 * so the caller proceeds to its own normal (fatal) panic. `pc` is the caller's own return address
 * (`__builtin_return_address(0)`), recorded as `TrapCatchInfo.rip`. */
bool archTrapCatchSoftware(uint64_t kind, uint64_t pc);

#endif
