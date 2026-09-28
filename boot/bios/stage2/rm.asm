; The real-mode thunk (D-102, docs/specs/bios-boot.md §4): rmInt()/rmIdle(), called from 32-bit C
; (rm.h). Lives in .text16 alongside entry.asm's A20/GDT code -- the segment-switching sequences
; below only work while every byte they touch (this code, the GDT, the low scratch area) is
; reachable with 16-bit real-mode addressing, i.e. below linear 0x10000 (stage2.ld ASSERTs this
; for the whole section).
extern gdtr

; RmRegs field byte offsets (rm.h) -- kept in sync with that struct's layout by the
; _Static_assert there, not re-checked here (this file has no way to see the C struct).
%define RMREGS_EAX    0
%define RMREGS_EBX    4
%define RMREGS_ECX    8
%define RMREGS_EDX    12
%define RMREGS_ESI    16
%define RMREGS_EDI    20
%define RMREGS_EBP    24
%define RMREGS_EFLAGS 28
%define RMREGS_DS     32
%define RMREGS_ES     34

section .text16

global rmInt
global rmIdle

; void rmInt(uint32_t intNo, RmRegs *r); cdecl.
bits 32
rmInt:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov eax, [ebp + 8]        ; intNo
    mov byte [rmIntNo], al
    mov esi, [ebp + 12]        ; r
    mov [rmRPtr], esi

    mov eax, [esi + RMREGS_EAX]
    mov [rmScratch + RMREGS_EAX], eax
    mov eax, [esi + RMREGS_EBX]
    mov [rmScratch + RMREGS_EBX], eax
    mov eax, [esi + RMREGS_ECX]
    mov [rmScratch + RMREGS_ECX], eax
    mov eax, [esi + RMREGS_EDX]
    mov [rmScratch + RMREGS_EDX], eax
    mov eax, [esi + RMREGS_ESI]
    mov [rmScratch + RMREGS_ESI], eax
    mov eax, [esi + RMREGS_EDI]
    mov [rmScratch + RMREGS_EDI], eax
    mov eax, [esi + RMREGS_EBP]
    mov [rmScratch + RMREGS_EBP], eax
    mov ax, [esi + RMREGS_DS]
    mov [rmScratch + RMREGS_DS], ax
    mov ax, [esi + RMREGS_ES]
    mov [rmScratch + RMREGS_ES], ax

    mov [rmSavedEsp], esp
    cli
    jmp 0x18:.pm16

bits 16
.pm16:
    mov ax, 0x20               ; data16 (D-102's GDT layout)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:.rm

.rm:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax                 ; fs stays 0 for the rest of this window: the caller's ds/es
    mov gs, ax                 ; (loaded below) must never shadow our own access to rmScratch/IVT
    mov ss, ax
    mov sp, 0x7C00              ; the real-mode stack (docs/specs/bios-boot.md's memory map);
                                  ; real mode only ever uses the low 16 bits of esp as SP, and
                                  ; the PM stack (rmSavedEsp) lives well above 0x10000

    mov eax, [rmScratch + RMREGS_EAX]
    mov ebx, [rmScratch + RMREGS_EBX]
    mov ecx, [rmScratch + RMREGS_ECX]
    mov edx, [rmScratch + RMREGS_EDX]
    mov esi, [rmScratch + RMREGS_ESI]
    mov edi, [rmScratch + RMREGS_EDI]
    mov ebp, [rmScratch + RMREGS_EBP]  ; clobbers this function's own frame pointer -- the
                                          ; epilogue below is written to never depend on it again
    ; es/ds last: after this, [rmScratch+...] via plain ds-relative addressing is no longer safe
    ; (ds now holds whatever the caller asked for), which is why fs (still 0) covers the IVT
    ; lookup and every scratch write from here on.
    mov ax, [rmScratch + RMREGS_ES]
    mov es, ax
    mov ax, [rmScratch + RMREGS_DS]
    mov ds, ax

    mov al, [fs:rmIntNo]
    movzx bx, al
    shl bx, 2

    sti
    pushf
    call far [fs:bx]            ; exact `int` emulation: pushf + a far call through the live IVT
                                  ; entry, read fresh every time (never cached) -- balances a
                                  ; handler that does `iret` (pops eip,cs,flags, undoing both
                                  ; pushes) or `retf 2` (pops eip,cs, then discards our pushed
                                  ; flags without restoring them)
    cli

    ; Capture every result register via mov (never touches FLAGS) before the pushf below, so the
    ; BIOS call's own resulting FLAGS survive intact until the very last thing we do with them.
    mov [fs:rmScratch + RMREGS_EAX], eax
    mov [fs:rmScratch + RMREGS_EBX], ebx
    mov [fs:rmScratch + RMREGS_ECX], ecx
    mov [fs:rmScratch + RMREGS_EDX], edx
    mov [fs:rmScratch + RMREGS_ESI], esi
    mov [fs:rmScratch + RMREGS_EDI], edi
    mov [fs:rmScratch + RMREGS_EBP], ebp
    mov [fs:rmScratch + RMREGS_DS], ds
    mov [fs:rmScratch + RMREGS_ES], es
    pushf
    pop cx
    mov [fs:rmScratch + RMREGS_EFLAGS], cx
    mov word [fs:rmScratch + RMREGS_EFLAGS + 2], 0

    lgdt [fs:gdtr]              ; BIOS code (e.g. a `call32`-style helper) may have replaced GDTR
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:.pmBack

bits 32
.pmBack:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, [rmSavedEsp]
    cld

    mov esi, [rmRPtr]
    mov eax, [rmScratch + RMREGS_EAX]
    mov [esi + RMREGS_EAX], eax
    mov eax, [rmScratch + RMREGS_EBX]
    mov [esi + RMREGS_EBX], eax
    mov eax, [rmScratch + RMREGS_ECX]
    mov [esi + RMREGS_ECX], eax
    mov eax, [rmScratch + RMREGS_EDX]
    mov [esi + RMREGS_EDX], eax
    mov eax, [rmScratch + RMREGS_ESI]
    mov [esi + RMREGS_ESI], eax
    mov eax, [rmScratch + RMREGS_EDI]
    mov [esi + RMREGS_EDI], eax
    mov eax, [rmScratch + RMREGS_EBP]
    mov [esi + RMREGS_EBP], eax
    mov eax, [rmScratch + RMREGS_EFLAGS]
    mov [esi + RMREGS_EFLAGS], eax
    mov ax, [rmScratch + RMREGS_DS]
    mov [esi + RMREGS_DS], ax
    mov ax, [rmScratch + RMREGS_ES]
    mov [esi + RMREGS_ES], ax

    ; No `mov esp, ebp` here (unlike a normal epilogue): the real-mode window above deliberately
    ; loaded the caller's requested RmRegs.ebp into the *live* EBP register (BIOS calls can take/
    ; return values in EBP), clobbering this function's own frame pointer -- so EBP can no longer
    ; be trusted to locate the stack frame. It's never needed for that anyway: rmSavedEsp already
    ; points at the top of this prologue's four pushes, so popping all four in order lands ESP
    ; exactly at the return address on its own. (A real, empirically-found bug: an earlier version
    ; of this epilogue used `mov esp, ebp` here and silently jumped into garbage on return.)
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; void rmIdle(void); cdecl.
bits 32
rmIdle:
    push ebp
    mov ebp, esp
    mov [rmSavedEsp], esp
    cli
    jmp 0x18:.pm16

bits 16
.pm16:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:.rm

.rm:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    hlt
    cli

    lgdt [fs:gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:.pmBack

bits 32
.pmBack:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, [rmSavedEsp]
    cld
    mov esp, ebp                ; safe here: rmIdle's real-mode window never touches ebp
    pop ebp
    ret

; Low-memory scratch (docs/specs/bios-boot.md's fixed layout) -- not reentrant, shared by every
; rmInt() call.
rmIntNo:    db 0
rmRPtr:     dd 0
rmSavedEsp: dd 0
align 4
rmScratch:  times 36 db 0
