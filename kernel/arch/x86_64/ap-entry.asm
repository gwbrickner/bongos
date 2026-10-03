; AP 64-bit entry (ARCHITECTURE §7.4, D-192). The trampoline jumps here in long mode on the
; trampoline's temporary page tables (the kernel half is a copy of the kernel PML4's), with
;   rsp = the AP's kernel stack top, rdi = its CpuLocal, rsi = the kernel PML4's physical address.
; IF=0, no IDT. Nothing here may touch memory outside the kernel half until CR3 is switched.
bits 64

section .text
extern apMain
extern bootGdtr
global apEntry64:function

apEntry64:
    ; The kernel's boot GDT is in the kernel half (mapped under the temporary tables), unlike the
    ; trampoline's own GDT, which lives in the low page the kernel PML4 does not map: reload the
    ; segment registers from it BEFORE the CR3 switch, since an interrupt reads CS from the GDT.
    lgdt [rel bootGdtr]
    push 0x08                          ; KERNEL_CS
    lea  rax, [rel .reloadCs]
    push rax
    o64 retf
.reloadCs:
    mov  ax, 0x10                      ; KERNEL_DS
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  eax, eax
    mov  fs, ax
    mov  gs, ax

    mov  cr3, rsi                      ; the kernel's own page tables

    ; IA32_GS_BASE = this CPU's CpuLocal (D-190); nothing reloads GS after this.
    mov  ecx, 0xC0000101
    mov  eax, edi
    mov  rdx, rdi
    shr  rdx, 32
    wrmsr
    mov  ecx, 0xC0000102               ; IA32_KERNEL_GS_BASE = 0
    xor  eax, eax
    xor  edx, edx
    wrmsr

    and  rsp, -16
    xor  ebp, ebp                      ; backtrace terminator
    call apMain                        ; _Noreturn void apMain(CpuLocal *cl), rdi still = cl
.hang:
    cli
    hlt
    jmp  .hang
