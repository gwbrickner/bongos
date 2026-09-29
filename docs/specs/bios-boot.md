# BIOS boot: on-disk format and stage1/stage2 ABI (M2.5, D-099..D-112)

This is the byte-level companion to ARCHITECTURE.md §5.1/§5.4/§5.6. Read those first for the
big picture; this doc has the exact layouts an implementer needs.

## 1. On-disk layout (D-099)

**Protective MBR (LBA 0), 512 bytes:**

| Offset | Size | Contents |
|---|---|---|
| 0x000 | 440 | stage1 code (`boot/bios/stage1.asm`, `nasm -f bin`, exactly 440 bytes) |
| 0x1A8 | 16 | **stage1 patch block** (mkimage writes this; stage1 reads it) |
| 0x1B8 | 4 | unique disk signature (0, unused) |
| 0x1BC | 2 | 0 |
| 0x1BE | 64 | one partition entry: the protective-MBR entry (type `0xEE`), written by `gptWriteLayout()` |
| 0x1FE | 2 | `55 AA` boot signature |

**stage1 patch block** (offset 0x1A8, 16 bytes -- mkimage patches this, stage1 must not use an
absolute address before it's read):

| Offset | Size | Field |
|---|---|---|
| +0 | u32 | magic `0x42503153` (`"S1PB"`) |
| +4 | u64 | `stage2Lba` (absolute 512-byte LBA of stage2's first sector) |
| +12 | u16 | `stage2Sectors` (512-byte sectors, <= 832) |
| +14 | u16 | reserved, 0 |

stage1's own code must end at or before offset 0x1A8; `boot/bios/stage1.asm` asserts this with
`times 0x1A8-($-$$) db 0` (a build failure, not a silent overrun, if stage1 grows past it).

**BIOS boot partition** (GPT partition #1, type `21686148-6449-6E6F-744E-656564454649`,
1 MiB, LBA 2048 by default): holds stage2 raw from its first sector, zero-padded to a whole
512-byte sector, size <= min(partition size, 0x68000 = 832 sectors = 416 KiB).

**stage2 header** (file offset 0, linear address 0x8000 once loaded):

| Offset | Size | Field |
|---|---|---|
| +0 | 2 | short jump to the 16-bit entry point |
| +2 | 2 | `90 90` (padding, keeps the magic 4-byte aligned) |
| +4 | u32 | magic `0x44483253` (`"S2HD"`) |
| +8 | u32 | `fileSize` (`__stage2FileEnd - 0x8000`, filled in by the linker/build, not mkimage) |
| +12 | u32 | `headerVersion` = 1 |

`mkimage --stage1 PATH --stage2 PATH` (both flags together or neither) validates both magics,
stage1's exact 440-byte size, stage2's `fileSize` against the real file size and the 832-sector
cap, then patches stage1's `stage2Lba`/`stage2Sectors` and installs both blobs. The patch step
is `mkimagePatchStage1(uint8_t code[440], uint64_t lba, uint32_t sectors)` in
`tools/mkimage/biosboot.c`, a pure function with its own host test.

## 2. stage1 behavior (D-100)

Entry state (BIOS): CS:IP = 0000:7C00 or 07C0:0000 (varies by firmware), DL = boot drive number,
everything else undefined.

1. `cli`; DS=ES=SS=0; SP=0x7C00; `cld`.
2. Relocate: copy 512 bytes from 0x7C00 to 0x0600 (`rep movsw`), then far-jump to `0000:relocated`
   (fixes both possible entry CS:IP conventions -- nothing before this jump may use an absolute
   address, since it doesn't yet know which convention this BIOS used).
3. `sti`; save DL (boot drive) in a fixed relocated-code location.
4. EDD check: INT 13h AH=41h, BX=0x55AA, DL=boot drive. Failure (CF=1, or BX != 0xAA55 on return,
   or CX bit 0 clear) is fatal -- error `E`.
5. Validate the patch block at 0x07A8 (relocated offset of 0x1A8): magic must be `S1PB`.
   `stage2Sectors` of 0 or > 832 is fatal -- error `P`.
6. Read stage2 in chunks of `min(remaining, 64)` sectors. Each chunk goes to a fresh 32 KiB-aligned
   32 KiB segment (`seg = 0x0800 + k*0x800, off = 0`) so no single transfer crosses a 64 KiB DMA
   boundary and stays under the 127-sector-per-call limit some BIOSes enforce. The INT 13h AH=42h
   Device Address Packet (EDD 3.0, 16 bytes: `u8 size=0x10, u8 0, u16 count, u16 off, u16 seg,
   u64 lba`) is rebuilt from scratch before every attempt (some BIOSes overwrite the count field
   with the actual transferred count). Up to 3 attempts per chunk (an AH=00h disk reset between
   failures); still failing after 3 is fatal -- error `R`, printed with the BIOS status byte
   (AH) from the failing call.
7. After the whole read loop, check the loaded copy's magic at linear 0x8004 equals `S2HD`;
   mismatch is fatal -- error `M`.
8. On any fatal error: print `"stage1 error " <code-char> <AH-as-2-hex-digits>` via INT 10h
   AH=0Eh teletype output, then `cli; hlt` in a loop. The status byte is only ever meaningful for
   error `R` (the read-retry path is the only one that saves the failing call's AH); errors `E`,
   `P`, and `M` print `00`, since nothing sets that byte before them.
9. On success: far-jump to `0000:8000` with DL = boot drive, DS=ES=SS=0, SP=0x7C00,
   SI = 0x07A8 (linear address of the relocated patch block), IF=1, DF=0.

## 3. stage2 memory layout (D-101)

Fixed low-memory scratch (all real-mode-reachable, all below 0x8000 except where noted):

| Range | Use |
|---|---|
| 0x0000-0x04FF | IVT/BDA -- never touched |
| 0x0500-0x0501 | A20 test word |
| 0x0600-0x07FF | dead stage1 (relocated copy, unused after handoff) |
| 0x1000-0x100F | INT 13h AH=42h Device Address Packet |
| 0x1100-0x1117 | E820 entry buffer (24 bytes) |
| 0x1180-0x119D | EDD AH=48h result buffer (0x1E bytes) |
| 0x1200-0x13FF | VBE `VbeInfoBlock` (512 bytes) |
| 0x1400-0x14FF | VBE `ModeInfoBlock` (256 bytes) |
| 0x2000-0x5FFF | disk bounce buffer (16 KiB = 32 sectors) |
| 0x6000-0x7BFF | real-mode stack (top at 0x7C00) |
| 0x8000- | stage2 image itself |

stage2's link layout (`boot/bios/stage2/stage2.ld`, base address 0x8000):

1. `.text16` -- the header (`KEEP(*(.text16.header))` first), the 16-bit entry, A20 code, the
   `gdtr` GDT-descriptor pointer (the 6-byte limit+base structure `lgdt` loads -- the GDT table
   itself lives in `.trampoline`, below), the real-mode thunk. Must end at or before linear
   0x10000 (`ASSERT(. <= 0x10000, "stage2 .text16 overflows the 16-bit-reachable region")`).
2. `.trampoline`, `ALIGN(4096)`, <= 4096 bytes (`ASSERT`ed): the protected-mode GDT, the
   long-mode trampoline, and its parameter block. Page-aligned because it is identity-mapped
   into the final page tables verbatim.
3. `.text`, `.rodata`, `.data` (32-bit C code and constants).
4. `__stage2FileEnd` (the symbol the header's `fileSize` field is computed from).
5. `.bss` (`NOLOAD`) -- zeroed by stage2 itself after the PM switch (not present in the flat
   binary file).
6. `.stack` (`NOLOAD`), 64 KiB, giving `__pmStackTop`.
7. `ASSERT(__stage2End <= 0x70000, "stage2 grew past its 416 KiB on-disk budget")`.
8. `/DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }`.

At the very start of the 16-bit entry, stage2 reads INT 12h's reported KiB and checks it against
`__stage2End`; too little low memory is a fatal, logged condition (this loader has no fallback).

All loader heap allocations (temporary file buffers, the kernel image, the boot stack, the
BootInfo/page-table pool) come from `bootheap` (D-107), which only ever hands out memory in
[1 MiB, 4 GiB) -- nothing is ever allocated from the low-memory scratch above.

## 4. GDT and the real-mode thunk ABI (D-102)

GDT (lives in `.trampoline`; every descriptor has its Accessed bit pre-set, so no descriptor
load can ever fault later once paging is on with WP=1):

| Selector | Descriptor |
|---|---|
| 0x00 | null |
| 0x08 | code32, base 0, limit 4 GiB, `0x00CF9B000000FFFF` |
| 0x10 | data32, base 0, limit 4 GiB, `0x00CF93000000FFFF` |
| 0x18 | code16, base 0, limit 64 KiB, `0x00009B000000FFFF` |
| 0x20 | data16, base 0, limit 64 KiB, `0x000093000000FFFF` |
| 0x28 | code64, `0x00AF9B000000FFFF` |

`boot/bios/stage2/rm.h` (the thunk's C-visible ABI):

```c
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, eflags;
    uint16_t ds, es;
} RmRegs;
_Static_assert(sizeof(RmRegs) == 36, "RmRegs layout is fixed by rm.asm");

void rmInt(uint32_t intNo, RmRegs *r);  /* runs INT intNo in real mode with *r loaded */
void rmIdle(void);                       /* sti; hlt; cli in real mode -- lets BIOS IRQs run */
```

`rmInt`: copies `*r` into the low-memory scratch area, drops to real mode, loads the given
registers, executes a **far call through the live IVT entry** for `intNo` (not a self-modifying
`int imm8`, not v86 mode -- an exact emulation of `int` that balances both `iret`- and
`retf 2`-returning BIOS handlers), copies the resulting registers plus CF/ZF (packed into the
returned `eflags` field) back into `*r`, restores PM, and returns. Neither `rmInt` nor `rmIdle`
is reentrant; both use the same fixed scratch area. IF=1 only inside the real-mode window it
opens; the 32-bit C code always runs with IF=0. GDTR is reloaded on every return to PM, since
BIOS code is free to replace it. **Not built** (D-114): there is no PM-only diagnostic IDT.
`IDTR` is never loaded in protected mode at all -- it still holds whatever the CPU's power-on
default or a prior real-mode `int`/`lidt` left it as (the live IVT: base 0, limit 0x3FF, read as
IDT gate descriptors once IDTR is consulted in PM), so a stray fault in this code almost
certainly triple-faults rather than producing a diagnostic. D-102's original description of a
32-gate diagnostic IDT here was aspirational and never implemented; deferred past M2.5.

## 5. Long-mode entry (D-111)

The trampoline lives in the same identity-mapped `.trampoline` page as the GDT (mapped
present-only, recorded as `LOADER_RECLAIM`). Sequence, run with paging off and IF=0:

1. Read its arguments (`pml4Phys`, and via a small parameter block: final `rsp`, the BootInfo
   virtual address, the kernel entry point) off the soon-to-be-unmapped PM stack **before**
   enabling paging.
2. `CR4 |= PAE | PGE`.
3. `CR3 = pml4Phys` (must be < 4 GiB -- guaranteed by the [1 MiB, 4 GiB) heap).
4. `EFER |= LME | NXE` (NXE before the first page-table walk that could see an NX bit).
5. `CR0 |= PG` (WP stays 0 for now).
6. Far jump to a 64-bit code selector (0x28) -- the GDT is reachable because this whole page is
   identity-mapped.
7. In 64-bit mode: reload DS/ES/SS to 0x10 (flat data), `CR0 |= WP`, load the final `rsp`, load
   `rdi` with the BootInfo physical-turned-virtual address, `xor ebp, ebp`, jump to the kernel
   entry point.

Before this runs, both legacy PICs are fully masked (`out 0x21, 0xFF; out 0xA1, 0xFF`) -- BIOS
leaves IRQ0-7 wired to interrupt vectors 8-15, which collide with the kernel's own exception
vectors (ARCHITECTURE §7.2), so anything left unmasked before the kernel installs its own IDT
is a hazard.

## 6. Owner-review flags

D-099 (on-disk format), D-109 (refines ARCHITECTURE §5.5's video-mode-selection rule to require
each color channel be <=8 bits), and D-111 (§5.4 BIOS addendum) all touch already-owner-reviewed
architecture and are called out again in the M2.5 PR per CLAUDE.md.
