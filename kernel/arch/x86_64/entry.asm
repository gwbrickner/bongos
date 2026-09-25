; Kernel entry stub (ARCHITECTURE §5.4, D-061/D-062). Assembled with `nasm -f elf64 -g -F dwarf`.
;
; State on entry (guaranteed by the loader, ARCHITECTURE §5.4):
;   rdi = BootInfo HHDM virtual address
;   rsp = top of the loader's own 64 KiB boot stack (HHDM)
;   IF=0; CS/SS and the IDTR still point at the firmware's own GDT/IDT, now unmapped under our
;   CR3 (the loader never installs one of its own -- ARCHITECTURE §5.4).
;
; Rule: until the stack switch below, touch no memory except RIP-relative kernel symbols, and
; never clobber rdi.
bits 64

section .text.entry progbits alloc exec nowrite align=16
global kernelEntry:function
extern kernelMain
extern kernelBootStackTop

kernelEntry:
    cli
    cld
    lea  rsp, [rel kernelBootStackTop]  ; our own stack first (16-aligned); rdi untouched

    lgdt [rel bootGdtr]
    push 0x08                          ; KERNEL_CS (ARCHITECTURE §7.1 selector layout)
    lea  rax, [rel .reloadCs]
    push rax
    o64 retf                           ; far return: reloads CS from our own GDT
.reloadCs:
    mov  ax, 0x10                      ; KERNEL_DS
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  eax, eax
    mov  fs, ax                        ; bases are set later via MSR (M2.1/M3); don't rely on them
    mov  gs, ax

    lidt [rel nullIdtr]                ; limit 0: any exception now triple-faults (no IDT yet)

    xor  ebp, ebp                      ; backtrace terminator (panic() stops here)
    call kernelMain                    ; _Noreturn void kernelMain(const BootInfo *bi)
    ; kernelMain replaces this temporary GDT/IDT with the real ones (archCpuTablesInit(),
    ; ARCHITECTURE §7.1/D-072) before doing anything else; this stub's tables cover only the
    ; serialInit()-and-earlier window.
.hang:                                 ; unreachable in practice (kernelMain never returns)
    cli
    hlt
    jmp .hang

; RW, not rodata: the CPU sets the Accessed bit on segment load if it's clear (SDM Vol 3A
; §3.4.5.1), which would #PF against a read-only GDT once CR0.WP=1 -- so every descriptor below
; already has it preset instead.
section .data
align 16
bootGdt:
    dq 0                    ; null descriptor
    dq 0x00AF9B000000FFFF   ; code64, base=0, limit=max, present, DPL0, L=1, Accessed
    dq 0x00CF93000000FFFF   ; data, base=0, limit=max, present, DPL0, D/B=1, Accessed
bootGdtr:
    dw 3 * 8 - 1
    dq bootGdt

section .rodata
nullIdtr:
    dw 0
    dq 0

; Zero-initialized (NOBITS): the loader's ELF loader contract already zero-fills p_memsz-p_filesz,
; so this needs no explicit clearing here. The guard page kernel.ld places just below this section
; is unmapped, turning a boot-stack overflow into a #PF instead of silent .bss corruption.
section .bootstack nobits alloc write noexec align=4096
    resb 16384
