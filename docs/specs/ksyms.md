# ksyms v1: the embedded kernel symbol table

D-073. Backs `symbolize()`/`backtracePrint()` (ARCHITECTURE §24) for panic reports. Built by
`tools/mksyms` from `kernel.elf`'s own ELF64 symtab as a post-link step, embedded in the `.ksyms`
linker section (`kernel/arch/x86_64/kernel.ld`), and decoded at runtime by the pure, dependency-
light `kernel/core/ksyms-decode.c` (also used unmodified by `tools/mksyms --check` and
`tests/host/ksyms_test.c`).

## Why a two-pass link

The table's own size can't be known until every kernel symbol exists, but the table has to be
linked *into* the kernel image. `mk/kernel.mk` links the kernel twice:

1. **Pass 1** links against an empty placeholder `.ksyms` blob. `tools/mksyms -o ksyms.bin
   kernel.pass1.elf` then derives the real table from pass 1's own symtab.
2. **Pass 2** links again, this time against `ksyms.bin`.
3. `tools/mksyms --check kernel.elf` re-derives the table a second time, from pass 2's own
   symtab, and byte-compares it against the `.ksyms` section pass 2 actually embedded. A
   mismatch fails the build.

This works because `.ksyms` lives in `.rodata`, which the linker script places *after* `.text`
(`kernel.ld`). Growing `.rodata` to hold the real (larger) blob never moves anything in `.text`,
so every symbol's address is identical between pass 1 and pass 2 -- the two derivations are
guaranteed to produce the same bytes, and `--check` turns that guarantee into a build-time proof
instead of an assumption.

Symbol offsets in the table are stored **relative to `kernelTextStart`**, not as absolute
addresses, so the table carries no relocations and needs no adjustment when M2.6's KASLR slides
the kernel (D-047).

## Wire format

All multi-byte fields are little-endian and read one byte at a time (`kernel/core/ksyms-decode.c`
never assumes any particular alignment of the blob).

### Header (32 bytes)

| Offset | Field | Notes |
|---|---|---|
| 0x00 | `magic` (u32) | `0x4D59534B` ("KSYM" as bytes) |
| 0x04 | `version` (u16) | `1` |
| 0x06 | `headerSize` (u16) | `32` |
| 0x08 | `count` (u32) | number of symbols, at most `1<<20` |
| 0x0C | `restartInterval` (u32) | power of two, 1-256; always `16` as written by `mksyms` |
| 0x10 | `addrsOff` (u32) | always equals `headerSize` (32) |
| 0x14 | `restartsOff` (u32) | `addrsOff + 8*count` |
| 0x18 | `namesOff` (u32) | `restartsOff + 4*ceil(count/restartInterval)` |
| 0x1C | `namesSize` (u32) | bytes of name data, padded to a multiple of 8 |

### `addrs[count]`

One `{offset: u32, size: u32}` pair per symbol, **strictly ascending by `offset`**, both relative
to `kernelTextStart`. Only `.text` `FUNC`/`NOTYPE` symbols are kept; when multiple input symbols
share an address, `mksyms` keeps exactly one, preferring (in order) `FUNC` over `NOTYPE`, then
`GLOBAL` over `WEAK` over `LOCAL` binding, then the shorter name, then bytewise order. A symbol
with `st_size == 0` in the input ELF gets its `size` filled in as the gap to the next kept
symbol's offset (or to the end of `.text` for the last one).

### `restarts[ceil(count/restartInterval)]`

One `u32` per restart point: the byte offset **relative to `namesOff`** of that entry's name data
in the `names` stream.

### `names`

A stream of front-coded entries, one per symbol in address order:

```
u8 shared      -- bytes shared with the previous symbol's name (0 at every restart point)
u8 suffixLen   -- bytes that follow, verbatim
<suffixLen bytes>
```

No name (`shared + suffixLen`) exceeds 255 bytes. Names are not NUL-terminated in the stream;
`ksymsLookup()` adds the terminator to its output buffer.

## Lookup algorithm (`ksymsLookup`, `kernel/core/ksyms-decode.c`)

1. Validate the header (magic, version, `headerSize`, `count`, `restartInterval`, and that
   `addrsOff`/`restartsOff`/`namesOff` land exactly where the count/interval predict) and every
   length against the blob's actual size. Any mismatch returns `STATUS_ERR_INVALID` rather than
   reading out of bounds.
2. Binary search `addrs` for the last entry whose `offset <= off`. If `off` isn't covered by that
   entry's `[offset, offset+size)`, return `STATUS_ERR_NOT_FOUND`.
3. Decode that entry's name by front-coding forward from its nearest restart point (at most
   `restartInterval - 1` steps).

This is pure, allocation-free, and bounded in time -- safe to call from any trap context,
including NMI, which is why `symbolize()` (the kernel-side wrapper that adds `kernelTextStart` to
get an absolute address) needs no locks.

## Symbolizing a return address

A call to a `_Noreturn` function is the last instruction of its caller-visible function, so the
raw return address pushed for that call can be the *first byte of the next symbol* rather than
an address inside the calling function. `backtracePrint()` therefore looks up every return
address (except the innermost, faulting PC) at `address - 1`, but prints the **offset from the
real, unadjusted address** -- so the printed line still shows exactly where control was when the
call happened, not one byte earlier.
