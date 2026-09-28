; BIOS stage2 skeleton (D-101, docs/specs/bios-boot.md). This is step 6 of the M2.5
; implementation plan: prove stage1 -> stage2 handoff actually works under real QEMU before any
; A20/protected-mode/thunk code exists. Assembled `nasm -f bin`, loaded at linear 0x8000 (this
; file's whole ORG). Writes a fixed banner straight to COM1 with raw port I/O (no C runtime, no
; loaderSerialInit -- that's boot/common/hw/serial.c, wired in once stage2 has a 32-bit C
; environment to call it from) and halts. Superseded incrementally by later commits: the real
; stage2 entry point, A20, the protected-mode switch, and the thunk all replace this file.
bits 16
org 0x8000

%define S2HD_MAGIC 0x44483253 ; "S2HD"

    jmp short entry
    db 0x90, 0x90
    dd S2HD_MAGIC
    dd stage2End - $$   ; fileSize: the assembler computes this file's own exact byte length
    dd 1                 ; headerVersion

entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    ; Raw 16550 UART init (same register sequence as boot/common/hw/serial.c's
    ; loaderSerialInit(), duplicated here only because this skeleton predates having a C
    ; environment to call that from at all).
    mov dx, 0x3F9
    xor al, al
    out dx, al           ; IER: disable UART interrupts
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al           ; LCR: DLAB on
    mov dx, 0x3F8
    mov al, 1
    out dx, al           ; divisor low byte: 1 -> 115200 baud
    mov dx, 0x3F9
    xor al, al
    out dx, al           ; divisor high byte
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al           ; LCR: 8N1, DLAB off
    mov dx, 0x3FA
    mov al, 0xC1
    out dx, al           ; FCR: FIFO enable, 14-byte trigger
    mov dx, 0x3FC
    mov al, 0x03
    out dx, al           ; MCR: DTR+RTS asserted

    mov si, msg
.loop:
    lodsb
    test al, al
    jz halt
    call serial_write_byte
    jmp .loop

halt:
    cli
    hlt
    jmp halt

; al = byte to send; clobbers ax, dx.
serial_write_byte:
    push ax
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20        ; LSR bit 5: THR empty
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    ret

msg: db "loader: stage2 real mode", 13, 10, 0

stage2End:
