; BIOS MBR stage1 (D-099/D-100, docs/specs/bios-boot.md). Assembled `nasm -f bin`, exactly
; BIOSBOOT_STAGE1_SIZE (440) bytes -- the protective MBR's boot-code area, ARCHITECTURE §5.1.
; Relocates itself to 0x0600, then uses INT 13h AH=42h (LBA extensions) to read stage2 into
; memory using the LBA/sector-count mkimage patches into the block at MBR offset 0x1A8
; (tools/mkimage/biosboot.c), and jumps to it.
;
; Every label from `relocated` onward is computed for execution at 0x0000:0x0600 (see the ORG
; below) -- correct only *after* the relocation copy runs, which is why the code up to and
; including the far jump to `relocated` uses no label references at all, only segment-independent
; immediate addresses, so it works whichever of the two BIOS entry conventions (0000:7C00 or
; 07C0:0000) this machine uses.
bits 16
org 0x0600

%define S1PB_MAGIC 0x42503153 ; "S1PB"
%define S2HD_MAGIC 0x44483253 ; "S2HD"

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256           ; 256 words = 512 bytes
    rep movsw

    jmp 0x0000:relocated

relocated:
    sti
    mov [bootDrive], dl

    ; EDD installation check (INT 13h AH=41h): CF=1, or BX not echoed back as 0xAA55, or the
    ; extensions bit (CX bit 0) clear, is fatal.
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [bootDrive]
    int 0x13
    jc err_edd
    cmp bx, 0xAA55
    jne err_edd
    test cl, 1
    jz err_edd

    cmp dword [patchBlock], S1PB_MAGIC
    jne err_patch
    mov ax, [patchSectors]
    test ax, ax
    jz err_patch
    cmp ax, 832
    ja err_patch

    mov [remaining], ax
    mov eax, [patchLba]
    mov [curLbaLo], eax
    mov eax, [patchLba + 4]
    mov [curLbaHi], eax
    mov word [curSeg], 0x0800

read_chunk:
    mov ax, [remaining]
    test ax, ax
    jz read_done
    cmp ax, 64
    jbe .haveCount
    mov ax, 64
.haveCount:
    mov [chunkCount], ax

    mov byte [retries], 3
read_attempt:
    ; Rebuild the DAP from scratch before every attempt: some BIOSes overwrite its count field
    ; with the number of sectors actually transferred.
    mov byte [dap], 0x10
    mov byte [dap + 1], 0
    mov ax, [chunkCount]
    mov [dap + 2], ax
    mov word [dap + 4], 0      ; buffer offset
    mov ax, [curSeg]
    mov [dap + 6], ax          ; buffer segment
    mov eax, [curLbaLo]
    mov [dap + 8], eax
    mov eax, [curLbaHi]
    mov [dap + 12], eax

    mov ah, 0x42
    mov dl, [bootDrive]
    mov si, dap
    int 0x13
    jnc read_ok

    mov [lastStatus], ah
    xor ah, ah                 ; AH=00h: reset the disk system before retrying
    mov dl, [bootDrive]
    int 0x13
    dec byte [retries]
    jnz read_attempt
    jmp err_read

read_ok:
    movzx eax, word [chunkCount]
    add dword [curLbaLo], eax
    mov ax, [chunkCount]
    shl ax, 5                   ; sectors * 512 / 16 = paragraphs per sector = 32
    add word [curSeg], ax
    mov ax, [remaining]
    sub ax, [chunkCount]
    mov [remaining], ax
    jmp read_chunk

read_done:
    cmp dword [0x8004], S2HD_MAGIC
    jne err_magic

    mov dl, [bootDrive]
    mov si, patchBlock
    jmp 0x0000:0x8000

err_edd:
    mov al, 'E'
    jmp report_error
err_patch:
    mov al, 'P'
    jmp report_error
err_read:
    mov al, 'R'
    jmp report_error
err_magic:
    mov al, 'M'

report_error:
    mov [errCode], al
    mov si, errMsg
print_msg:
    lodsb
    test al, al
    jz print_code
    mov ah, 0x0E
    xor bh, bh
    mov bl, 7
    int 0x10
    jmp print_msg
print_code:
    mov al, [errCode]
    mov ah, 0x0E
    int 0x10
    mov al, ' '
    mov ah, 0x0E
    int 0x10
    mov al, [lastStatus]
    call print_hex_byte
hang:
    cli
    hlt
    jmp hang

print_hex_byte:
    push ax
    mov ah, al
    shr al, 4
    call print_hex_nibble
    mov al, ah
    and al, 0x0F
    call print_hex_nibble
    pop ax
    ret

print_hex_nibble:
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .out
.digit:
    add al, '0'
.out:
    mov ah, 0x0E
    int 0x10
    ret

errMsg:      db "stage1 error ", 0
bootDrive:   db 0
remaining:   dw 0
curLbaLo:    dd 0
curLbaHi:    dd 0
curSeg:      dw 0
chunkCount:  dw 0
retries:     db 0
lastStatus:  db 0
errCode:     db 0

dap:
    db 0x10, 0
    dw 0    ; count
    dw 0    ; buffer offset
    dw 0    ; buffer segment
    dd 0    ; LBA low
    dd 0    ; LBA high

; Pad up to the patch block mkimage writes into (absolute MBR offset 0x1A8): a build failure, not
; a silent overrun, if the code above ever grows past it.
times 0x1A8 - ($ - $$) db 0

patchBlock:
    dd S1PB_MAGIC
patchLba:
    dq 0
patchSectors:
    dw 0
    dw 0    ; reserved

times 440 - ($ - $$) db 0
