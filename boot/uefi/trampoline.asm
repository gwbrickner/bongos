; The final CR3-swap-and-jump (ARCHITECTURE §5.5 step 10, D-066). Assembled with `nasm -f win64`
; (COFF, matching the PE32+ loader's other objects) and linked into BOOTX64.EFI; the page holding
; it is identity-mapped R-X in the new page tables (boot/uefi/handoff.c) so it stays executable the
; instant CR3 changes, and it never touches any stack (the firmware's is unmapped under the new
; CR3, and ours isn't live yet) until `mov rsp, r8`.
;
; MS x64 ABI: _Noreturn void loaderTrampoline(uint64_t cr3 /*rcx*/, uint64_t bootInfoVa /*rdx*/,
;                                             uint64_t stackTopVa /*r8*/, uint64_t entryVa /*r9*/);
bits 64
section .text code align=64

global loaderTrampoline
global loaderTrampolineEnd

align 64
loaderTrampoline:
    cli
    cld
    mov  rax, cr4
    and  rax, ~(1 << 7)   ; PGE=0: flushes every TLB entry, including the firmware's globals
    mov  cr4, rax
    mov  cr3, rcx         ; NO stack access until `mov rsp, r8` below
    or   rax, (1 << 7)    ; PGE=1
    mov  cr4, rax
    mov  rax, cr0
    bts  rax, 16          ; CR0.WP=1
    mov  cr0, rax
    mov  rsp, r8          ; the kernel's HHDM boot-stack top, 16-aligned
    mov  rdi, rdx
    xor  ebp, ebp
    jmp  r9
loaderTrampolineEnd:

%if (loaderTrampolineEnd - loaderTrampoline) > 64
%error "loaderTrampoline grew past 64 bytes: it must stay well inside a single 4 KiB page"
%endif
