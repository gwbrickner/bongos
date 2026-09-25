; Loads the real per-CPU GDT/TR/IDT built by cpu-tables.c (ARCHITECTURE §7.1, D-072). Named
; apart from cpu-tables.c on purpose: kernel.mk builds both into build/kernel/kernel/arch/x86_64/,
; and a .c and .asm sharing a basename would collide on the same output .o path.
bits 64

section .text
global archLoadGdt:function
global archLoadTr:function
global archLoadIdt:function

; void archLoadGdt(const void *gdtr)  -- rdi = pointer to a 10-byte GDTR (limit:2, base:8)
archLoadGdt:
    lgdt [rdi]
    push 0x08                  ; KERNEL_CS
    lea  rax, [rel .reloadCs]
    push rax
    o64 retf                   ; far return: reloads CS from the new GDT
.reloadCs:
    mov  ax, 0x10               ; KERNEL_DS
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  eax, eax               ; FS/GS bases are set later via MSR (M3.5); selectors stay null
    mov  fs, ax
    mov  gs, ax
    ret

; void archLoadTr(uint16_t selector)  -- di = selector
archLoadTr:
    ltr  di
    ret

; void archLoadIdt(const void *idtr)  -- rdi = pointer to a 10-byte IDTR (limit:2, base:8)
archLoadIdt:
    lidt [rdi]
    ret

; The three IST stacks (ARCHITECTURE §7.1, D-072): 16 KiB each, NOBITS (zero-initialized by the
; loader's ELF loader contract, same reasoning as entry.asm's .bootstack), each in kernel.ld's
; own PT_LOAD with an unmapped guard page below it.
section .iststack1 nobits alloc write noexec align=4096
    resb 16384

section .iststack2 nobits alloc write noexec align=4096
    resb 16384

section .iststack3 nobits alloc write noexec align=4096
    resb 16384
