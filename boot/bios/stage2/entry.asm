; BIOS stage2 entry (D-101/D-102/D-103, docs/specs/bios-boot.md). Linked (not a flat `nasm -f bin`
; blob any more, now that stage2 has a 32-bit C part, D-104): `nasm -f elf32`, then
; `ld.lld -T stage2.ld`, then `objcopy -O binary`. 16-bit real-mode bring-up (A20, the GDT, the
; switch to 32-bit protected mode) lives here; `stage2Main` (C) takes over from `pm_entry`.
bits 16

%define S2HD_MAGIC 0x44483253 ; "S2HD"

extern __stage2FileEnd
extern __bssStart
extern __bssEnd
extern stage2Main
global entry16
global gdtr
global bootDrive

section .text16.header progbits alloc exec nowrite align=1
    jmp short entry16
    db 0x90, 0x90
    dd S2HD_MAGIC
    dd __stage2FileEnd - 0x8000 ; fileSize: link-time constant (both symbols are absolute)
    dd 1                          ; headerVersion

section .text16 progbits alloc exec nowrite align=1

entry16:
    mov [bootDrive], dl       ; captured first, before anything below can touch dl (D-100 step 9:
                                ; stage1 hands off with DL = boot drive; disk.c's thunked INT 13h
                                ; calls need it and have no other way to learn it)
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    call enable_a20
    test al, al
    jnz .a20ok
    mov al, 'A'
    jmp fatal16
.a20ok:

    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm_entry

; ---- A20 (D-100/D-103): fast gate, then INT 15h AX=2401h, then the 8042, each re-verified by
; the classic wrap-around test. Returns al=1 once enabled, al=0 if every method failed. ----
enable_a20:
    call check_a20
    test al, al
    jnz .done
    call enable_a20_fast
    call check_a20
    test al, al
    jnz .done
    mov ax, 0x2401
    int 0x15
    call check_a20
    test al, al
    jnz .done
    call enable_a20_8042
    call check_a20
.done:
    ret

; Writes two different words to 0000:0500 and FFFF:0510 (the same physical address, 0x100500, iff
; A20 is disabled) and reads 0000:0500 back: unchanged means A20 is enabled (al=1), overwritten by
; the aliased write means it's still disabled (al=0). Clobbers ax, cx, es.
check_a20:
    mov word [0x0500], 0x1234
    mov cx, 0xFFFF
    mov es, cx
    mov word [es:0x0510], 0x4321
    mov ax, [0x0500]
    xor cx, cx
    mov es, cx             ; restore es=0 (every other use of es in this file assumes that)
    cmp ax, 0x1234
    je .enabled
    xor al, al
    ret
.enabled:
    mov al, 1
    ret

enable_a20_fast:
    in al, 0x92
    or al, 0x02            ; enable the A20 fast-gate bit
    and al, 0xFE            ; clear bit 0: on some chipsets, leaving it set triggers a fast reset
    out 0x92, al
    ret

enable_a20_8042:
    call kbc_wait_input_empty
    mov al, 0xAD            ; disable keyboard
    out 0x64, al
    call kbc_wait_input_empty
    mov al, 0xD0            ; read controller output port
    out 0x64, al
    call kbc_wait_output_full
    in al, 0x60
    mov bl, al
    call kbc_wait_input_empty
    mov al, 0xD1             ; write controller output port
    out 0x64, al
    call kbc_wait_input_empty
    mov al, bl
    or al, 0x02              ; set the A20 bit
    out 0x60, al
    call kbc_wait_input_empty
    mov al, 0xAE              ; re-enable keyboard
    out 0x64, al
    call kbc_wait_input_empty
    ret

; Bounded (not infinite) waits: real broken hardware could otherwise hang stage2 forever with no
; diagnostic. A timeout just means this method silently doesn't take effect; enable_a20's
; check_a20 call afterward is what actually decides whether it worked.
kbc_wait_input_empty:
    push cx
    xor cx, cx
.loop:
    in al, 0x64
    test al, 0x02
    jz .done
    dec cx
    jnz .loop
.done:
    pop cx
    ret

kbc_wait_output_full:
    push cx
    xor cx, cx
.loop:
    in al, 0x64
    test al, 0x01
    jnz .done
    dec cx
    jnz .loop
.done:
    pop cx
    ret

; al = one-character error code. Prints "stage2 error " <al> via INT 10h teletype and halts.
fatal16:
    mov [errCode16], al       ; a memory var, not a register: INT 10h teletype clobbers ah/bx
    mov si, fatalMsg
.printMsg:
    lodsb
    test al, al
    jz .printCode
    mov ah, 0x0E
    xor bh, bh
    mov bl, 7
    int 0x10
    jmp .printMsg
.printCode:
    mov al, [errCode16]
    mov ah, 0x0E
    xor bh, bh
    mov bl, 7
    int 0x10
.hang:
    cli
    hlt
    jmp .hang

fatalMsg:   db "stage2 error ", 0
errCode16:  db 0
bootDrive:  db 0

; The GDT itself lives in .trampoline (D-102/D-111), not here: once paging is on, any segment
; reload or far jump reads its descriptor out of GDTR.base as a *linear* address, so the table
; must be inside the one page the final page tables identity-map -- a copy left in .text16 would
; fault the instant CR0.PG=1. gdtr itself stays in .text16 (below 0x10000): entry16's `lgdt [gdtr]`
; above and rm.asm's `lgdt [fs:gdtr]` both use 16-bit real-mode addressing to reach it. `dd gdt`
; (a single symbol's address) is an ordinary relocation, but the *limit* can't be computed the same
; way `dd __stage2FileEnd - 0x8000` is elsewhere in this file: that's symbol-minus-*constant*
; (one relocation with a folded addend), while `gdtEnd - gdt` would be symbol-minus-symbol across
; two different object files (trampoline.asm) -- NASM has no relocation for that and refuses to
; assemble it ("expression is not simple or relocatable"). The GDT is a fixed, 6-entry, 8-byte-
; descriptor table (D-102) that never changes at runtime, so its size is just hard-coded instead.
extern gdt
gdtr:
    dw 6 * 8 - 1               ; limit = (6 descriptors * 8 bytes each) - 1; keep in sync with the
                                 ; entry count in trampoline.asm's `gdt:` table if that ever changes
    dd gdt

section .text progbits alloc exec nowrite align=16
bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x00090000       ; a temporary PM stack; replaced once stage2 has its own (D-101)

    ; .bss is NOLOAD (never present in the flat on-disk image, D-101/§5.6 step 3) -- every static
    ; C variable in it (the E820/heap/handoff arrays main.c and handoff.c use) starts as whatever
    ; garbage happened to be in physical memory otherwise. Zero it before anything touches it.
    cld
    mov edi, __bssStart
    mov ecx, __bssEnd
    sub ecx, edi
    xor eax, eax
    rep stosb

    call stage2Main            ; loaderSerialInit() there does the UART setup (D-104)
.hang:                        ; unreachable: stage2Main is _Noreturn
    cli
    hlt
    jmp .hang
