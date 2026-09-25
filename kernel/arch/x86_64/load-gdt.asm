; archLoadGdt (ARCHITECTURE §7.1, D-072): loads a new GDT and reloads every segment register from
; it. CS can only be reloaded via a far jump/call/return (SDM Vol 3A §3.4.2), which C's inline asm
; can express but this project keeps that trick in one place -- entry.asm's own temporary-GDT
; switch already does the exact same sequence; this is the permanent GDT's turn to do it. Named
; apart from cpu-init.c (not cpu-init.asm) so their object files don't collide at the same
; build/.../cpu-init.o path.
;
; void archLoadGdt(const X86DescriptorPtr *gdtr); -- rdi = gdtr (SysV AMD64 ABI)
;
; Selectors below are raw values matching kernel/arch/x86_64/include/gdt.h's GDT_SEL_KERNEL_CS
; (0x08) / GDT_SEL_KERNEL_DS (0x10) -- NASM can't include that C header, so keep them in sync by
; hand (same convention entry.asm's own temporary GDT switch already uses).
bits 64

section .text
global archLoadGdt:function
archLoadGdt:
    lgdt [rdi]
    push 0x08                          ; GDT_SEL_KERNEL_CS
    lea  rax, [rel .reloadCs]
    push rax
    o64 retf                           ; far return: reloads CS from the new GDT
.reloadCs:
    mov  ax, 0x10                      ; GDT_SEL_KERNEL_DS
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  eax, eax                      ; fs/gs bases are set later via MSR (M2.1 doesn't need them yet)
    mov  fs, ax
    mov  gs, ax
    ret
