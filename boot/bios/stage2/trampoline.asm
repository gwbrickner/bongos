; The long-mode entry trampoline (D-111, docs/specs/bios-boot.md §5). Runs entirely inside one
; page-aligned, <=4096-byte page (stage2.ld's `.trampoline` section) that the final page tables
; identity-map (PT_FLAGS_TRAMPOLINE, R-X, present-only) before this is ever called -- everything
; this code and its data touch from the moment CR0.PG=1 onward must be inside that same page, since
; nothing else is mapped yet and there is no fault handler that could survive the alternative.
;
; The GDT lives here too (D-102), not in .text16 where entry.asm originally built it: once paging
; is on, a far jump or segment reload reads its descriptor out of GDTR.base as a *linear* address,
; so the table has to be inside the one page guaranteed to still be reachable. gdtr itself (in
; entry.asm, .text16) already points at `gdt` here via a link-time-resolved cross-section address,
; so no `lgdt` reload happens in this file -- the GDTR loaded back in entry16 is still the active
; one, and rm.asm's own `lgdt [cs:gdtr]` (after every thunk round trip) keeps pointing at the same
; table for exactly this reason.
bits 32

global bootTrampoline
global gdt

; void bootTrampoline(uint64_t pml4Phys, uint64_t bootInfoVa, uint64_t stackTopVa, uint64_t entryVa);
; cdecl, _Noreturn. Called from stage2Main's ordinary 32-bit C code (still running on the temporary
; PM stack, entry.asm's pm_entry) -- everything up to and including the last real-mode-thunk call
; must already be done by the time this runs: the real-mode window rmInt()/rmIdle() open needs the
; legacy PICs unmasked and IF=1, both of which this function tears down for good on its way to long
; mode (docs/specs/bios-boot.md's PIC-masking note).
section .trampoline progbits alloc exec write align=4096

bootTrampoline:
    cli
    cld

    ; Copy every argument off the (about-to-become-unreachable-under-paging) caller's stack into
    ; the parameter block below, all still in this same identity-mapped page, before touching
    ; CR4/CR3/EFER/CR0 (D-111 step 1). cdecl pushes a uint64_t as two dwords, low word first;
    ; pml4Phys's high dword is never read here since the [1 MiB, 4 GiB) heap the caller allocated
    ; it from guarantees it's already < 4 GiB (the C caller checks this explicitly before calling,
    ; not just assumed here).
    mov eax, [esp+12]          ; bootInfoVa low
    mov [tpBootInfo], eax
    mov eax, [esp+16]          ; bootInfoVa high
    mov [tpBootInfo+4], eax
    mov eax, [esp+20]          ; stackTopVa low
    mov [tpStackTop], eax
    mov eax, [esp+24]          ; stackTopVa high
    mov [tpStackTop+4], eax
    mov eax, [esp+28]          ; entryVa low
    mov [tpEntry], eax
    mov eax, [esp+32]          ; entryVa high
    mov [tpEntry+4], eax
    mov ebx, [esp+4]           ; pml4Phys low (the only half we need)

    ; Both legacy PICs fully masked (docs/specs/bios-boot.md's long-mode-entry note): BIOS leaves
    ; IRQ0-7 wired to interrupt vectors 8-15, colliding with the kernel's own exception vectors,
    ; and nothing after this point ever calls rmInt()/rmIdle() again (both need IRQs unmasked and
    ; IF=1 during their real-mode window) -- safe to mask for good here.
    mov al, 0xFF
    out 0xA1, al
    out 0x21, al

    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)  ; PAE, PGE
    and eax, ~(1 << 12)          ; LA57 stays off: this loader only ever builds 4-level tables
    mov cr4, eax

    mov cr3, ebx

    mov ecx, 0xC0000080          ; IA32_EFER -- rdmsr/wrmsr clobber eax/edx, safe now that pml4
    rdmsr                         ; (ebx) and every argument (tp* memory) are already captured
    or eax, (1 << 8) | (1 << 11) ; LME, NXE (NXE before the first page-table walk that could see
    wrmsr                         ; an NX leaf)

    mov eax, cr0
    or eax, 0x80000000           ; PG (PE is already 1; WP stays 0 until 64-bit mode)
    mov cr0, eax

    jmp dword 0x28:.longMode      ; flushes the prefetch queue and loads a code64 CS -- the CPU is
                                    ; in IA-32e mode the instant CR0.PG committed above, but stays
                                    ; in 32-bit compatibility submode until this reloads CS with an
                                    ; L-bit-set descriptor

bits 64
.longMode:
    mov eax, 0x10                 ; the existing data32 descriptor works unchanged as a 64-bit
    mov ds, ax                     ; data selector: base/limit are ignored in long mode, and this
    mov es, ax                     ; one is already writable/DPL0, all SS needs
    mov ss, ax
    xor eax, eax                  ; fs/gs left at a known value (0) rather than whatever real mode
    mov fs, ax                     ; happened to leave in them -- the kernel sets up its own later
    mov gs, ax

    mov rax, cr0
    bts rax, 16                   ; WP, now that every page table this code depends on is final
    mov cr0, rax

    mov rsp, [tpStackTop]         ; a single 64-bit load: the two dwords above were written as one
    mov rdi, [tpBootInfo]          ; little-endian qword each, low dword first, matching how a
                                    ; 64-bit `mov` reads them back
    mov rax, [tpEntry]
    xor ebp, ebp                  ; a clean (0) frame-pointer chain for the kernel's own unwinder
    jmp rax                       ; entryVa is a runtime value (KASLR-slid in a later milestone),
                                    ; so this must be an indirect jump, not a link-time-constant one

align 8
tpBootInfo: dq 0
tpStackTop: dq 0
tpEntry:    dq 0

; The GDT (D-102): every descriptor's Accessed bit is pre-set so no descriptor load can ever fault
; once paging is on with CR0.WP=1 (this page is mapped read-only, PT_FLAGS_TRAMPOLINE -- a "would
; the CPU write to it" question the Accessed bit answers "no" to before it's ever asked).
align 8
gdt:
    dq 0                      ; null
    dq 0x00CF9B000000FFFF     ; 0x08: code32, base 0, limit 4 GiB
    dq 0x00CF93000000FFFF     ; 0x10: data32, base 0, limit 4 GiB
    dq 0x00009B000000FFFF     ; 0x18: code16, base 0, limit 64 KiB
    dq 0x000093000000FFFF     ; 0x20: data16, base 0, limit 64 KiB
    dq 0x00AF9B000000FFFF     ; 0x28: code64
gdtEnd:

; entry.asm's `gdtr` can't compute `gdtEnd - gdt` itself (a cross-object-file symbol difference
; NASM has no relocation for -- see the comment there) and hard-codes `6 * 8 - 1` instead. Guard
; against that drifting from the table above with two `times` lines whose repeat counts are each
; other's negation: both assemble to nothing ("times 0 db 0") when the two agree, but if an entry
; is added (table grows), the first line's count goes negative and fails to assemble; if an entry
; is removed (table shrinks), the first line would silently pad with zero bytes instead of
; catching it, so the second line (whose count goes negative in exactly that case) catches it
; instead.
times (6 * 8) - (gdtEnd - gdt) db 0
times (gdtEnd - gdt) - (6 * 8) db 0
