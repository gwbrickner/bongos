/* The real-mode thunk (D-102, docs/specs/bios-boot.md §4): lets stage2's 32-bit C code make a
 * real BIOS interrupt call. Neither rmInt() nor rmIdle() is reentrant, and both use a fixed
 * low-memory scratch area -- never call either from an interrupt handler or concurrently. */
#ifndef BOOT_BIOS_STAGE2_RM_H
#define BOOT_BIOS_STAGE2_RM_H

#include <stdint.h>

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, eflags;
    uint16_t ds, es;
} RmRegs;
_Static_assert(sizeof(RmRegs) == 36, "RmRegs layout is fixed by rm.asm's byte offsets");

/* Runs `intNo` in real mode with every field of `*r` loaded into the matching register/segment
 * first, then writes every result register/segment plus the resulting FLAGS (zero-extended into
 * `eflags`; CF is bit 0, ZF is bit 6, matching real x86 FLAGS bit positions -- no repacking)
 * back into `*r`. Implemented as an exact `int` emulation (a far call through the live IVT entry,
 * not a self-modifying `int imm8` and not v86 mode), so it balances whichever return convention
 * the BIOS handler uses (`iret` or `retf 2`). IF=1 only inside the real-mode window this opens;
 * the caller is always resumed with IF=0. Not reentrant. */
void rmInt(uint32_t intNo, RmRegs *r);

/* Drops to real mode, `sti; hlt; cli`, and returns -- letting any pending BIOS-owned real-mode
 * IRQ (timer, keyboard, ...) actually run once, then resumes with IF=0 as usual. Not reentrant. */
void rmIdle(void);

#endif
