; The 32 CPU exception entry stubs and their common dispatch trampoline (ARCHITECTURE §7.1,
; D-072). One 16-byte-stride stub per vector (padded with int3 so a stub that ever grows past 16
; bytes is a build-time error, not a silently mis-strided IDT): pushes a dummy error code for the
; ten vectors the CPU doesn't push one for itself, then the vector number, then falls into the
; shared isrCommon.
;
; ERRCODE_MASK bit v is set iff the CPU pushes a real error code for vector v (SDM Vol 3A,
; "Exception and Interrupt Reference"): 8 #DF, 10 #TS, 11 #NP, 12 #SS, 13 #GP, 14 #PF, 17 #AC,
; 21 #CP, 29 #VC, 30 #SX.
%define ERRCODE_MASK 0x60227D00

bits 64

section .text
extern archTrapDispatch
global archIsrStubs:data
global isrCommon:function

archIsrStubs:
%assign v 0
%rep 32
isrStub %+ v:
%if ((ERRCODE_MASK >> v) & 1) == 0
    push 0
%endif
    push v
    jmp  isrCommon
    times 16-($-isrStub %+ v) int3
%assign v v+1
%endrep

; Stack on entry: [vector][errorCode][RIP][CS][RFLAGS][RSP][SS] (low to high address), with
; interrupts already off (interrupt gate) and CS still kernel (M2.1 is CPL0-only; no swapgs yet).
isrCommon:
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
    cld                          ; SysV requires DF=0; iretq restores whatever DF the trap frame has

    ; A fake stack frame record so a plain rbp-chain backtrace taken from inside
    ; archTrapDispatch continues naturally into the interrupted code: [rbp+0] = the interrupted
    ; rbp (still live in the register, untouched since the push above only copied it), [rbp+8] =
    ; the interrupted RIP. 17*8 = 136 = offsetof(TrapFrame, rip) once the 15 GPRs above are on the
    ; stack (kernel/arch/x86_64/include/trap-frame.h).
    push qword [rsp + 17*8]
    push rbp
    mov  rbp, rsp
    lea  rdi, [rsp + 16]         ; TrapFrame * (skips the two fake-frame words above)
    call archTrapDispatch
    add  rsp, 16                 ; drop the fake frame

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
    add  rsp, 16                 ; drop vector + errorCode
    iretq
