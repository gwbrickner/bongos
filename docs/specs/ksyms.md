# KSYM v1: the kernel's embedded symbol table

D-075. A from-scratch, versioned, little-endian format holding the kernel's own `STT_FUNC`
symbols (name + address), used by `ksymSymbolize()` (panic/trap backtraces, ARCHITECTURE §24) to
turn a raw return address into `name+0xoff`. Built by the host tool `tools/ksyms` from the
kernel's own linked ELF (a two-pass link, see below) and decoded by `kernel/core/ksym.c`.

## Layout

All integers are little-endian. Offsets are relative to the start of the blob.

### Header (64 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u32 | magic | `0x4D59534B` ("KSYM" as 4 ASCII bytes, LE) |
| 4 | u16 | version | 1 |
| 6 | u16 | headerSize | 64 |
| 8 | u32 | count | number of symbols |
| 12 | u32 | blockShift | 6 (64 symbols per block index entry) |
| 16 | u64 | textBase | link-time `kernelTextStart` |
| 24 | u64 | textEnd | link-time `kernelTextEnd` |
| 32 | u32 | indexOffset | byte offset of the block index |
| 36 | u32 | indexCount | `ceil(count / 64)` |
| 40 | u32 | tokenOffset | byte offset of the token directory |
| 44 | u32 | tokenCount | number of BPE tokens (0-128) |
| 48 | u32 | streamOffset | byte offset of the symbol stream |
| 52 | u32 | streamSize | byte length of the symbol stream |
| 56 | u64 | reserved | 0 |

An empty table (`count == 0`) is a valid 64-byte blob: `indexCount`/`tokenCount`/`streamSize` are
all 0, and `indexOffset`/`tokenOffset`/`streamOffset` all equal 64 (the point right after the
header, with nothing there).

### Block index (`indexCount` * 16 bytes)

One entry per 64 consecutive symbols (address order), for a binary-searchable start point:

| Offset | Type | Field |
|---|---|---|
| +0 | u64 | firstAddr — the block's first symbol's absolute address |
| +8 | u32 | streamOff — byte offset into the stream (relative to `streamOffset`) where that symbol's entry begins |
| +12 | u32 | reserved (0) |

### Token directory (`tokenCount` * 4 bytes, then the pool)

Byte-pair compression tokens (values 0x80-0xFF in the symbol stream), each entry:

| Offset | Type | Field |
|---|---|---|
| +0 | u16 | poolOff — byte offset into the pool (relative to the pool's own start, `tokenOffset + tokenCount*4`) |
| +2 | u8 | len — the token's flattened expansion length, 1-32 bytes |
| +3 | u8 | reserved (0) |

The pool holds every token's expansion back-to-back: raw literal bytes only (a token's expansion
is never itself token-encoded -- see "Compression" below).

### Symbol stream (`streamSize` bytes)

One entry per symbol, in ascending address order:

- `ULEB128 delta`: 0 for a block's first symbol; otherwise `addr[i] - addr[i-1]`.
- `u8 encLen`: the encoded name's length, 1-255.
- `encLen` bytes: the encoded name. A byte `< 0x80` is a literal ASCII character; a byte `>= 0x80`
  is token `byte - 0x80` (expand recursively -- a token's own expansion is always literal bytes,
  so this never recurses more than one level deep).

## Compression

Byte-pair encoding over the pool of all symbol names (7-bit ASCII, so bytes 0x80-0xFF are free to
use as token IDs). The encoder (`tools/ksyms/ksyms-encode.c`) runs up to 128 rounds; each round:

1. Count every adjacent byte-pair across every symbol's current (possibly already
   partially-tokenized) byte sequence.
2. Among pairs occurring at least 4 times *and* whose flattened expansion would be at most 32
   bytes, pick the highest count (ties broken by the smaller `(a << 8) | b` value).
3. Stop if no pair qualifies.
4. Assign it the next token ID (`0x80 + tokenCount`), record its flattened expansion, and replace
   every occurrence left to right (non-overlapping) in every sequence with that one token byte.

## Lookup

`ksymDecodeLookup(blob, size, addr, ...)` (`kernel/core/ksym.c`, pure and host-testable):

1. Reject `count == 0`, or `addr < textBase` or `addr >= textEnd`, with NOT_FOUND.
2. Binary-search the block index for the last block whose `firstAddr <= addr`.
3. Linearly decode that block's symbols (at most 64) from the stream, accumulating `delta` into
   the running address, until the address that's `<= addr` and closest to it is found (or the
   next block/end of stream is reached).
4. Decode that symbol's name (expanding tokens) into the caller's buffer.

Every read is bounds-checked against `size`; a malformed or truncated blob returns NOT_FOUND
rather than reading out of bounds or faulting -- this runs during a panic, when trust in the
kernel's own state is already reduced.

## Build (two-pass link)

`kernel.elf`'s own final symbol addresses have to be embedded in that same `kernel.elf` -- solved
the same way `tools/mkfont`'s `c` subcommand embeds the console font, but with an extra pass since
here the *input* to the generator is the very thing being built:

1. `tools/ksyms empty > build/gen/ksyms-empty.c` -- a valid, empty placeholder blob.
2. Link `build/kernel/kernel.pass1.elf` with the kernel objects plus that placeholder.
3. `tools/ksyms gen build/kernel/kernel.pass1.elf > build/gen/ksyms.c` -- the real blob, from
   pass 1's own symbol addresses (text/rodata/data layout is identical between the two passes,
   since `.ksyms` lives in `.rodata` *after* `.text`, so no function's address changes between
   passes).
4. Link the real `build/kernel/kernel.elf` with the kernel objects plus the real `ksyms.o`.
5. `tools/ksyms check build/kernel/kernel.elf` -- re-extracts and re-encodes that final ELF's own
   symbols and byte-compares the result against what's actually in its `.ksyms` section. A
   mismatch fails the build (and deletes the bad ELF) rather than shipping a symbol table that
   could point at the wrong function.
