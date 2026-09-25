; IDT trap stubs (ARCHITECTURE §7.2, D-074): one stub per vector (0-255), %rep-generated so there
; are no 256 hand-written near-duplicate blocks. Every stub normalizes the stack to [vector,
; errorCode, <hardware frame>] -- the CPU only pushes a real error code for a handful of vectors
; (SDM Vol 3A Table 6-1; AMD APM Vol 2 Sec8.2 for #VC/#SX), so the rest get a fake 0 pushed in its
; place, keeping trapCommon's stack layout (and TrapFrame, trap-frame.h) uniform for every vector.
bits 64

section .text

; void trapDispatch(TrapFrame *f); -- C handler, kernel/arch/x86_64/trap.c
extern trapDispatch

global trapStubsStart:function
global trapStubsEnd:function
trapStubsStart:

%assign v 0
%rep 256
align 16
trapStub %+ v:
  %if (v = 8) || (v = 10) || (v = 11) || (v = 12) || (v = 13) || (v = 14) || (v = 17) || (v = 21) || (v = 29) || (v = 30)
    push qword v                     ; the CPU already pushed a real error code for this vector
  %else
    push qword 0                     ; fake error code, so every vector's frame has the same shape
    push qword v
  %endif
    jmp trapCommon
%assign v v+1
%endrep

trapStubsEnd:

; Common trampoline every stub jumps into. `cld` because SysV requires DF=0 across a C call and
; nothing before this point can be trusted to have left it clear. -mno-red-zone (already a kernel
; build flag) is load-bearing here: the stub pushed directly below the interrupted RSP, so a red
; zone would have been clobbered by the CPU's own hardware frame push before we ever got control.
global trapCommon:function (trapCommon.end - trapCommon)
trapCommon:
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    ; 15 GPRs + vector + errorCode + 5 hardware-frame qwords = 22 qwords = 176 bytes pushed since
    ; the exception entered (SDM: RSP was 16-aligned right before the CPU pushed SS), so RSP is
    ; still 16-aligned here -- exactly what the SysV ABI requires at a `call`.
    mov  rdi, rsp                     ; TrapFrame* (trap-frame.h)
    call trapDispatch
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    add  rsp, 16                      ; discard vector + errorCode
    iretq
.end:

; archTrapCatch's setjmp/longjmp-style register save/resume (ARCHITECTURE §23, D-078). TrapCatchCtx
; (kernel/arch/x86_64/trap.c) is 7 qwords in exactly this order -- rsp, rbx, rbp, r12, r13, r14,
; r15 -- at offsets 0/8/16/24/32/40/48; this file and that struct must stay in sync by hand.
;
; uint32_t archTrapCatchCall(TrapCatchCtx *ctx, void (*fn)(void *), void *arg);
; rdi=ctx, rsi=fn, rdx=arg (SysV AMD64 ABI). Saves every callee-saved register `fn` (or anything it
; calls) might clobber, then calls `fn(arg)`. Returns 0 if `fn` returned normally. If a fault or a
; software trip (archTrapCatchSoftware) redirects execution to archTrapCatchResume below instead,
; *that* call "returns" here a second time with eax=1 -- ctx->rsp was saved *before* the local
; `push rbp` here, so restoring it unwinds straight back past this function's own prologue to
; exactly this point, without ever executing this function's own epilogue for real.
global archTrapCatchCall:function
archTrapCatchCall:
    mov  [rdi], rsp
    mov  [rdi+8], rbx
    mov  [rdi+16], rbp
    mov  [rdi+24], r12
    mov  [rdi+32], r13
    mov  [rdi+40], r14
    mov  [rdi+48], r15
    push rbp
    mov  rbp, rsp                     ; RSP stays 16-aligned; the backtrace chain stays walkable
    mov  rdi, rdx                     ; fn's one argument
    call rsi
    pop  rbp
    xor  eax, eax
    ret

; void archTrapCatchResume(TrapCatchCtx *ctx); -- rdi=ctx. Restores every register archTrapCatchCall
; saved, including RSP, then `ret`s -- which pops whatever archTrapCatchCall's *own* return address
; was (saved as part of ctx->rsp's snapshot), transferring control back to archTrapCatchCall's
; caller as if archTrapCatchCall had just returned normally, except with eax=1. Never returns to
; its own caller in the ordinary sense (its C declaration is `_Noreturn` for exactly this reason):
; called either from trapDispatch() after redirecting a TrapFrame's RIP/RSP/RDI here (so the CPU's
; own `iretq` is what actually transfers control into this function), or directly from
; archTrapCatchSoftware() for a software-raised catch.
global archTrapCatchResume:function
archTrapCatchResume:
    mov  rsp, [rdi]
    mov  rbx, [rdi+8]
    mov  rbp, [rdi+16]
    mov  r12, [rdi+24]
    mov  r13, [rdi+32]
    mov  r14, [rdi+40]
    mov  r15, [rdi+48]
    mov  eax, 1
    ret

; One pointer per vector, in vector order, for the C-side IDT builder (kernel/arch/x86_64/trap.c)
; to read gate offsets from without 256 `extern` declarations. R_X86_64_64 relocations, so this
; stays correct under KASLR once the kernel is relocatable (M2.6).
section .rodata
global trapStubTable:data (trapStubTableEnd - trapStubTable)
trapStubTable:
%assign v 0
%rep 256
    dq trapStub %+ v
%assign v v+1
%endrep
trapStubTableEnd:
