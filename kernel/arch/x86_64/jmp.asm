; A minimal setjmp/longjmp (ARCHITECTURE §7.1/D-076), kernel-private: used only by ktest.c's
; expected-panic recovery. ArchJmpBuf layout (kernel/include/arch/jmp.h): 8 uint64_t words --
; rbx, rbp, r12, r13, r14, r15, rsp (the value it should hold on return), return RIP.
bits 64

section .text
global archJmpSave:function
global archJmpRestore:function

; int archJmpSave(ArchJmpBuf *buf)  -- rdi = buf
archJmpSave:
    mov  [rdi + 0*8], rbx
    mov  [rdi + 1*8], rbp
    mov  [rdi + 2*8], r12
    mov  [rdi + 3*8], r13
    mov  [rdi + 4*8], r14
    mov  [rdi + 5*8], r15
    lea  rax, [rsp + 8]         ; rsp as it will be right after this function returns
    mov  [rdi + 6*8], rax
    mov  rax, [rsp]             ; the return address this call pushed
    mov  [rdi + 7*8], rax
    xor  eax, eax
    ret

; _Noreturn void archJmpRestore(ArchJmpBuf *buf, int val)  -- rdi = buf, esi = val
archJmpRestore:
    mov  rbx, [rdi + 0*8]
    mov  rbp, [rdi + 1*8]
    mov  r12, [rdi + 2*8]
    mov  r13, [rdi + 3*8]
    mov  r14, [rdi + 4*8]
    mov  r15, [rdi + 5*8]
    mov  rcx, [rdi + 6*8]       ; target rsp, staged in a register that survives the rsp switch
    mov  rdx, [rdi + 7*8]       ; target rip, same reasoning
    mov  eax, esi
    test eax, eax
    jnz  .nonzero
    mov  eax, 1                 ; archJmpSave() must appear to return nonzero
.nonzero:
    mov  rsp, rcx
    jmp  rdx
