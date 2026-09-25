; Assembly helpers for kernel/arch/x86_64/ktests.c (ARCHITECTURE §7.1, D-072/D-076). Only linked
; in by the debug/ktest build path in spirit (KERNEL_C_SOURCES always includes ktests.c, but its
; KTEST()s are simply never selected outside `ktest=`).
bits 64

section .text
extern kernelBootStackBottom

global archKtestInt3Clobber:function
global archKtestInt3Resume:function
global archKtestDoubleFault:function

; void archKtestInt3Clobber(uint64_t out[14])  -- rdi = out
;
; Loads every GPR except rbp/rsp with a distinct pattern, executes `int3`, then (at
; archKtestInt3Resume, where the CPU resumes after the non-fatal #BP -- ARCHITECTURE §7.1/D-072)
; records each register's value into `out[14]` in this order: rax, rbx, rcx, rdx, rsi, rdi, r8,
; r9, r10, r11, r12, r13, r14, r15. Both rbp and rsp are provably untouched by a trap (isr.asm's
; isrCommon fully restores them before `iretq`), so they stay valid as ordinary frame/stack
; pointers throughout -- no red-zone assumptions, since the kernel builds with -mno-red-zone: all
; scratch space is reserved with an explicit `sub rsp` *before* `int3`, so the CPU's own
; hardware-pushed trap frame (which lands at the current rsp) can never land on top of it.
archKtestInt3Clobber:
    push rbp
    mov  rbp, rsp
    push rbx                    ; [rbp-8]  true rbx (restored before return)
    push r12                    ; [rbp-16] true r12
    push r13                    ; [rbp-24] true r13
    push r14                    ; [rbp-32] true r14
    push r15                    ; [rbp-40] true r15
    push rdi                    ; [rbp-48] the `out` pointer (rdi is about to be clobbered)
    sub  rsp, 112                ; scratch region for the post-resume register dump, [rbp-160..rbp-49]

    mov  rax, 0x1111111111111111
    mov  rbx, 0x2222222222222222
    mov  rcx, 0x3333333333333333
    mov  rdx, 0x4444444444444444
    mov  rsi, 0x5555555555555555
    mov  rdi, 0x6666666666666666
    mov  r8,  0x7777777777777777
    mov  r9,  0x8888888888888888
    mov  r10, 0x9999999999999999
    mov  r11, 0xAAAAAAAAAAAAAAAA
    mov  r12, 0xBBBBBBBBBBBBBBBB
    mov  r13, 0xCCCCCCCCCCCCCCCC
    mov  r14, 0xDDDDDDDDDDDDDDDD
    mov  r15, 0xEEEEEEEEEEEEEEEE

    int3

archKtestInt3Resume:
    mov  [rbp - 56],  rax
    mov  [rbp - 64],  rbx
    mov  [rbp - 72],  rcx
    mov  [rbp - 80],  rdx
    mov  [rbp - 88],  rsi
    mov  [rbp - 96],  rdi
    mov  [rbp - 104], r8
    mov  [rbp - 112], r9
    mov  [rbp - 120], r10
    mov  [rbp - 128], r11
    mov  [rbp - 136], r12
    mov  [rbp - 144], r13
    mov  [rbp - 152], r14
    mov  [rbp - 160], r15

    mov  rax, [rbp - 48]         ; out pointer
    mov  rcx, [rbp - 56]
    mov  [rax + 0*8],  rcx       ; out[0]  = rax
    mov  rcx, [rbp - 64]
    mov  [rax + 1*8],  rcx       ; out[1]  = rbx
    mov  rcx, [rbp - 72]
    mov  [rax + 2*8],  rcx       ; out[2]  = rcx
    mov  rcx, [rbp - 80]
    mov  [rax + 3*8],  rcx       ; out[3]  = rdx
    mov  rcx, [rbp - 88]
    mov  [rax + 4*8],  rcx       ; out[4]  = rsi
    mov  rcx, [rbp - 96]
    mov  [rax + 5*8],  rcx       ; out[5]  = rdi
    mov  rcx, [rbp - 104]
    mov  [rax + 6*8],  rcx       ; out[6]  = r8
    mov  rcx, [rbp - 112]
    mov  [rax + 7*8],  rcx       ; out[7]  = r9
    mov  rcx, [rbp - 120]
    mov  [rax + 8*8],  rcx       ; out[8]  = r10
    mov  rcx, [rbp - 128]
    mov  [rax + 9*8],  rcx       ; out[9]  = r11
    mov  rcx, [rbp - 136]
    mov  [rax + 10*8], rcx       ; out[10] = r12
    mov  rcx, [rbp - 144]
    mov  [rax + 11*8], rcx       ; out[11] = r13
    mov  rcx, [rbp - 152]
    mov  [rax + 12*8], rcx       ; out[12] = r14
    mov  rcx, [rbp - 160]
    mov  [rax + 13*8], rcx       ; out[13] = r15

    mov  rbx, [rbp - 8]
    mov  r12, [rbp - 16]
    mov  r13, [rbp - 24]
    mov  r14, [rbp - 32]
    mov  r15, [rbp - 40]
    mov  rsp, rbp
    pop  rbp
    ret

; void archKtestDoubleFault(void) -- never returns. Sets rsp to the top of the unmapped guard
; page just below the boot stack (kernel.ld/D-061), then pushes: the write faults (#PF), and
; delivering that #PF needs to push its own hardware frame at the same bad rsp, which faults
; again -- a real #DF, delivered on IST1 (D-072). ktest-only; wedges the boot stack, so the
; calling ktest must have armed ktestExpectPanic("exception #DF") first.
archKtestDoubleFault:
    lea  rsp, [rel kernelBootStackBottom]
    push rax
    ud2
