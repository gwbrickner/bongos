/* Link layout for BIOS stage2 (D-101, docs/specs/bios-boot.md §3). stage2 is loaded flat at
 * linear 0x8000; `objcopy -O binary` turns the linked ELF into exactly the bytes this script lays
 * out (no ELF headers survive). `.text16*` (the header, the 16-bit entry, A20, and the real-mode
 * thunk) must stay real-mode-addressable; `.trampoline` (the GDT plus the long-mode entry
 * trampoline, D-102/D-111) is page-aligned since it's identity-mapped verbatim into the final
 * kernel page tables it help build -- everything the trampoline touches from the moment paging
 * turns on must live inside this one page, since nothing else is mapped yet. */
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

    .trampoline ALIGN(4096) : {
        __trampolineStart = .;
        KEEP(*(.trampoline))
        __trampolineEnd = .;
    }
    ASSERT((__trampolineStart & 0xFFF) == 0, "trampoline not page-aligned")
    ASSERT(__trampolineEnd - __trampolineStart <= 4096, ".trampoline exceeds one page")

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
        __bssStart = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bssEnd = .;
    }

    /* The 32-bit PM stack (D-101). Previously a hard-coded `mov esp, 0x00090000` in entry.asm --
     * a real EBDA-collision risk on hardware whose EBDA starts as low as 0x80000 (D-113 accepts
     * that as a sane EBDA base), since a stack growing down from 0x90000 would run straight into
     * it. Placing the stack here instead, immediately after stage2's own .bss and well below any
     * EBDA D-113 would accept as legitimate, avoids the collision structurally rather than by
     * hoping the BIOS's EBDA happens to be high enough. entry16 (entry.asm) also cross-checks
     * __stage2End against INT 12h's reported low-memory size before ever switching to PM, in
     * case a BIOS reports less conventional memory than that for some other reason. */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        __pmStackBottom = .;
        . += 0x10000; /* 64 KiB (D-101) */
        __pmStackTop = .;
    }

    __stage2End = .;
    ASSERT(__stage2End <= 0x70000, "stage2 (incl. .bss/.stack) grew past its 416 KiB budget (D-101)")

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
    }
}
