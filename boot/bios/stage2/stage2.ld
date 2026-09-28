/* Link layout for BIOS stage2 (D-101, docs/specs/bios-boot.md §3). stage2 is loaded flat at
 * linear 0x8000; `objcopy -O binary` turns the linked ELF into exactly the bytes this script lays
 * out (no ELF headers survive). `.text16*` (the header, the 16-bit entry, A20, the GDT, and later
 * the real-mode thunk) must stay real-mode-addressable; `.trampoline` (added once stage2 builds
 * page tables and enters long mode) is page-aligned since it's identity-mapped into the kernel's
 * own page tables verbatim. */
ENTRY(entry16)

SECTIONS {
    . = 0x8000;

    .text16.header : {
        KEEP(*(.text16.header))
    }
    .text16 : {
        *(.text16)
    }
    ASSERT(. <= 0x10000, "stage2 .text16 overflows the 16-bit-reachable region")

    .text : {
        *(.text)
        *(.text.*)
    }
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    .data : {
        *(.data)
        *(.data.*)
    }

    __stage2FileEnd = .;

    .bss (NOLOAD) : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    __stage2End = .;
    ASSERT(__stage2End <= 0x70000, "stage2 grew past its 416 KiB on-disk budget")

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
    }
}
