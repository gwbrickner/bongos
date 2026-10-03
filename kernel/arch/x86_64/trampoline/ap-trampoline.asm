; AP startup trampoline (ARCHITECTURE §7.4, D-192). Assembled flat (`nasm -f bin`, org 0), embedded in
; the kernel by ap-blob.asm and copied by the BSP into one 4 KiB page below 1 MiB. A SIPI starts the
; AP in real mode at CS = page >> 4, IP = 0. The page layout (mirrored by smp-impl.h's ApTrampData,
; which has _Static_asserts on every offset):
;   0x000  code (real -> protected -> long mode)
;   0xE00  GDT: null, code32 0x08, data32 0x10, code64 0x18
;   0xF00  ApTrampData: the BSP-filled parameter block and the AP's progress words
;
; The code has no relocations and no kernel addresses, so it is KASLR-proof; the three fields the
; blob prefills (gdtr base, the two far-jump offsets) are page-relative and the BSP adds the page's
; physical address once after the copy. The AP's identity mapping of this page is read-only/
; executable (D-192), so the last write to the page is `stage = 3`, before paging is enabled.
;
; Register use: EDI = the page's physical address from the real-mode prologue on (CPUID clobbers
; EBX, so not EBX); ESI is free.
%include "ap-tramp.inc"

bits 16
org 0

start:
    cli
    cld
    mov  ax, cs
    mov  ds, ax
    movzx edi, ax
    shl  edi, 4                     ; edi = this page's physical address

    ; Only the AP the BSP woke may run: compare our initial APIC id with targetApicId (leaf 0xB's
    ; x2APIC id when the BSP runs in x2APIC mode, else CPUID.1:EBX[31:24]).
    cmp  dword [DATA + TD_USE_LEAF_B], 0
    jne  .leafB
    mov  eax, 1
    cpuid
    shr  ebx, 24
    mov  eax, ebx
    jmp  .haveId
.leafB:
    mov  eax, 0xB
    xor  ecx, ecx
    cpuid
    mov  eax, edx
.haveId:
    cmp  eax, [DATA + TD_TARGET_APIC_ID]
    jne  park

    mov  eax, 1
    xchg eax, [DATA + TD_CLAIMED]   ; implicitly locked: one AP only runs the rest
    test eax, eax
    jnz  park

    mov  dword [DATA + TD_STAGE], 1
    o32 lgdt [DATA + TD_GDTR]

    ; INIT leaves CR0 = 0x60000010 (CD and NW set): an AP left that way runs uncached. Clear both.
    mov  eax, cr0
    and  eax, 0x9FFFFFFF
    or   eax, 1                     ; PE
    mov  cr0, eax
    jmp  dword far [DATA + TD_PM32_PTR]

park:
    cli
.hlt:
    hlt
    jmp  .hlt

bits 32
pm32:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  eax, eax
    mov  fs, ax
    mov  gs, ax
    mov  dword [edi + DATA + TD_STAGE], 2

    mov  eax, cr4
    or   eax, 0xA0                  ; PAE | PGE
    mov  cr4, eax
    mov  eax, [edi + DATA + TD_PML4_PHYS]
    mov  cr3, eax
    mov  ecx, 0xC0000080            ; IA32_EFER
    rdmsr
    or   eax, 0x900                 ; LME | NXE (without NXE every kernel PTE's bit 63 is reserved)
    wrmsr
    mov  dword [edi + DATA + TD_STAGE], 3   ; the last write to this page
    mov  eax, cr0
    or   eax, 0x80010000            ; PG | WP
    mov  cr0, eax
    jmp  dword far [edi + DATA + TD_LM64_PTR]

bits 64
lm64:
    mov  ebx, edi                   ; the upper halves of the GPRs are undefined after the switch
    mov  rsp, [rbx + DATA + TD_STACK_TOP]
    mov  rdi, [rbx + DATA + TD_CPU_LOCAL]
    mov  rsi, [rbx + DATA + TD_KERNEL_CR3]
    mov  rax, [rbx + DATA + TD_ENTRY64]
    jmp  rax                        ; apEntry64 (ap-entry.asm); never returns

; --- GDT at 0xE00 ---------------------------------------------------------------------------------
times GDT_OFF - ($ - $$) db 0
gdt:
    dq 0
    dq 0x00CF9B000000FFFF           ; 0x08 code32: base 0, limit 4 GiB, present, DPL0, accessed
    dq 0x00CF93000000FFFF           ; 0x10 data32
    dq 0x00AF9B000000FFFF           ; 0x18 code64: L=1
gdtEnd:

; --- parameter block at 0xF00 (the blob's prefilled values; the BSP fills the rest) ---------------
times DATA - ($ - $$) db 0
    dd TD_MAGIC_VALUE               ; magic
    dd 0                            ; claimed
    dd 0                            ; stage
    dd 0                            ; targetApicId
    dd 0                            ; useLeafB
    dd 0                            ; pml4Phys
    dw gdtEnd - gdt - 1             ; gdtr.limit
    dd GDT_OFF                      ; gdtr.base (page-relative; the BSP adds the page address)
    dw 0
    dd pm32                         ; pm32Ptr.off (page-relative)
    dw 0x08                         ; pm32Ptr.sel
    dw 0
    dd lm64                         ; lm64Ptr.off (page-relative)
    dw 0x18                         ; lm64Ptr.sel
    dw 0
    dq 0                            ; entry64
    dq 0                            ; stackTop
    dq 0                            ; cpuLocal
    dq 0                            ; kernelCr3
times 4096 - ($ - $$) db 0
