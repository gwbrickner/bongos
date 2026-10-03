; Offsets shared by ap-trampoline.asm; smp-impl.h's ApTrampData mirrors them with _Static_asserts.
GDT_OFF             equ 0xE00
DATA                equ 0xF00
TD_MAGIC_VALUE      equ 0x52545041          ; "APTR" little-endian
TD_MAGIC            equ 0x00
TD_CLAIMED          equ 0x04
TD_STAGE            equ 0x08
TD_TARGET_APIC_ID   equ 0x0C
TD_USE_LEAF_B       equ 0x10
TD_PML4_PHYS        equ 0x14
TD_GDTR             equ 0x18
TD_PM32_PTR         equ 0x20
TD_LM64_PTR         equ 0x28
TD_ENTRY64          equ 0x30
TD_STACK_TOP        equ 0x38
TD_CPU_LOCAL        equ 0x40
TD_KERNEL_CR3       equ 0x48
