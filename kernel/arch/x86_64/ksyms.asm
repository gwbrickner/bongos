; Embeds the ksyms blob (docs/specs/ksyms.md, D-073) into the kernel image's .ksyms section.
; Assembled twice by mk/kernel.mk's two-pass link, each time with a different KSYMS_BLOB path
; (-D on the nasm command line): the empty placeholder for pass 1, the real table (built by
; tools/mksyms from pass 1's own kernel.elf) for pass 2.
bits 64

section .ksyms progbits alloc noexec nowrite align=8
    incbin KSYMS_BLOB
