/* Portable-facing non-local jump primitive (a minimal, kernel-private setjmp/longjmp; D-076).
 * Used only by kernel/test/ktest.c's expected-panic recovery mechanism -- never for ordinary
 * kernel control flow. The real implementation is x86-specific (kernel/arch/x86_64/jmp.asm). */
#ifndef KERNEL_INCLUDE_ARCH_JMP_H
#define KERNEL_INCLUDE_ARCH_JMP_H

#include <stdint.h>

/* Opaque to callers beyond its size: rbx, rbp, r12-r15, rsp, and the return RIP (8 callee-saved
 * words). */
typedef struct ArchJmpBuf {
    uint64_t regs[8];
} ArchJmpBuf;

/* Saves the callee-saved registers and the calling function's return point into `*buf`, then
 * returns 0. A later archJmpRestore(buf, val) makes this same call site "return" again, this
 * time with archJmpRestore's `val` (forced nonzero). Must be called directly from the function
 * whose call site should be revisited -- like setjmp, calling it through another function and
 * jumping back after that function has already returned is undefined. No locks, IRQ-safe. */
__attribute__((returns_twice)) int archJmpSave(ArchJmpBuf *buf);

/* Restores the registers saved in `*buf` and jumps to its saved return point, making the
 * corresponding archJmpSave() call site return `val` (coerced to nonzero if it's 0). `*buf`'s
 * saved frame must still be live on the stack (i.e. the function that called archJmpSave must
 * not have returned yet). No locks; never returns. */
_Noreturn void archJmpRestore(ArchJmpBuf *buf, int val);

#endif
