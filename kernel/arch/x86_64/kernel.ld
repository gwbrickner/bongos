/* Kernel linker script (ARCHITECTURE §5.1/§6.1/§6.3, D-061). Produces exactly 4 PT_LOADs (R+X,
 * R, RW, and RW/NOBITS for the boot stack), each W^X. Link with:
 *   ld.lld -T kernel/arch/x86_64/kernel.ld -nostdlib -static --emit-relocs \
 *          -z max-page-size=0x1000 --build-id=none --orphan-handling=error
 * --orphan-handling=error is deliberate: any unexpected alloc section (.got, .init_array, ...)
 * fails the link instead of landing silently in a segment with the wrong permissions. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(kernelEntry)

KERNEL_LINK_BASE       = 0xFFFFFFFF80000000;
KERNEL_WINDOW_SIZE     = 0x20000000; /* 512 MiB, ARCHITECTURE §6.1 */
KERNEL_BOOT_STACK_SIZE = 0x4000;     /* 16 KiB, ARCHITECTURE §6.2 */

PHDRS {
    text   PT_LOAD FLAGS(5); /* R X */
    rodata PT_LOAD FLAGS(4); /* R   */
    data   PT_LOAD FLAGS(6); /* R W */
    stack  PT_LOAD FLAGS(6); /* R W, NOBITS; an unmapped guard page sits below it */
}

SECTIONS {
    . = KERNEL_LINK_BASE;
    kernelImageStart = .;

    kernelTextStart = .;
    .text : {
        *(.text.entry)
        *(.text .text.*)
    } :text
    . = ALIGN(4096);
    kernelTextEnd = .;

    kernelRodataStart = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    .ktests : ALIGN(8) {
        ktestsStart = .;
        KEEP(*(.ktests))
        ktestsEnd = .;
    } :rodata
    . = ALIGN(4096);
    kernelRodataEnd = .;

    kernelDataStart = .;
    .data : {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
    } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    . = ALIGN(4096);
    kernelDataEnd = .;

    . += 4096; /* unmapped guard page below the boot stack */
    .bootstack (NOLOAD) : ALIGN(4096) {
        kernelBootStackBottom = .;
        KEEP(*(.bootstack))
        kernelBootStackTop = .;
    } :stack
    . = ALIGN(4096);
    kernelImageEnd = .;

    /DISCARD/ : {
        *(.eh_frame) *(.eh_frame_hdr) *(.note) *(.note.*) *(.comment)
        *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash)
    }

    .symtab   0 : { *(.symtab) }
    .strtab   0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }

    .debug_info        0 : { *(.debug_info) }
    .debug_abbrev      0 : { *(.debug_abbrev) }
    .debug_line        0 : { *(.debug_line) }
    .debug_line_str    0 : { *(.debug_line_str) }
    .debug_str         0 : { *(.debug_str) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .debug_addr        0 : { *(.debug_addr) }
    .debug_rnglists    0 : { *(.debug_rnglists) }
    .debug_loclists    0 : { *(.debug_loclists) }
    .debug_ranges      0 : { *(.debug_ranges) }
    .debug_loc         0 : { *(.debug_loc) }
    .debug_frame       0 : { *(.debug_frame) }
    .debug_aranges     0 : { *(.debug_aranges) }
    .debug_pubnames    0 : { *(.debug_pubnames) }
    .debug_pubtypes    0 : { *(.debug_pubtypes) }
    .debug_names       0 : { *(.debug_names) }
    .debug_macinfo     0 : { *(.debug_macinfo) }
    .debug_macro       0 : { *(.debug_macro) }
}

ASSERT(kernelTextStart == KERNEL_LINK_BASE, "kernel.ld: .text must start at KERNEL_LINK_BASE")
ASSERT(kernelImageEnd - kernelImageStart <= KERNEL_WINDOW_SIZE,
       "kernel.ld: image exceeds the 512 MiB kernel window")
ASSERT(kernelBootStackTop - kernelBootStackBottom == KERNEL_BOOT_STACK_SIZE,
       "kernel.ld: boot stack must be exactly 16 KiB")
