; BIOS stage2 entry (D-101/D-102/D-103, docs/specs/bios-boot.md). Linked (not a flat `nasm -f bin`
; blob any more, now that stage2 has a 32-bit C part, D-104): `nasm -f elf32`, then
; `ld.lld -T stage2.ld`, then `objcopy -O binary`. 16-bit real-mode bring-up (A20, the GDT, the
; switch to 32-bit protected mode) lives here; `stage2Main` (C) takes over from `pm_entry`.
bits 16

%define S2HD_MAGIC 0x44483253 ; "S2HD"

extern __stage2FileEnd
extern stage2Main
global entry16

section .text16.header progbits alloc exec nowrite align=1
    jmp short entry16
    db 0x90, 0x90
    dd S2HD_MAGIC
    dd __stage2FileEnd - 0x8000 ; fileSize: link-time constant (both symbols are absolute)
    dd 1                          ; headerVersion

section .text16 progbits alloc exec nowrite align=1

entry16:
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

align 8
gdt:
    dq 0                      ; null
    dq 0x00CF9B000000FFFF     ; 0x08: code32, base 0, limit 4 GiB
    dq 0x00CF93000000FFFF     ; 0x10: data32, base 0, limit 4 GiB
    dq 0x00009B000000FFFF     ; 0x18: code16, base 0, limit 64 KiB
    dq 0x000093000000FFFF     ; 0x20: data16, base 0, limit 64 KiB
    dq 0x00AF9B000000FFFF     ; 0x28: code64
gdtr:
    dw ($ - gdt) - 1          ; limit = (gdt table size in bytes) - 1
    dd gdt

section .text progbits alloc exec nowrite align=16
bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x00090000       ; a temporary PM stack; replaced once stage2 has its own (D-101)

    call stage2Main            ; loaderSerialInit() there does the UART setup (D-104)
.hang:                        ; unreachable: stage2Main is _Noreturn
    cli
    hlt
    jmp .hang
