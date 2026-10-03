; Embeds the flat AP trampoline (trampoline/ap-trampoline.asm, built to ap-trampoline.bin by
; mk/kernel.mk) in the kernel's .rodata (ARCHITECTURE §7.4, D-192). The BSP copies these 4096 bytes
; into the trampoline page below 1 MiB.
bits 64

section .rodata
global apTrampolineBlob
global apTrampolineBlobEnd
align 16
apTrampolineBlob:
    incbin "ap-trampoline.bin"
apTrampolineBlobEnd:
