# bongfs v1: the native filesystem

**Status:** Draft, awaiting owner review (M7.5). Nothing here is binding until the owner approves
the M7.5 PR; after that, changes follow the DECISIONS.md rules (a new `D-0xx` row, plus an
ARCHITECTURE update in the same PR).

This is the byte-level companion to ARCHITECTURE §15.1 (D-019: custom journaling filesystem,
ordered mode, CRC32C). The proposed decisions are listed as `D-0xx.1`..`D-0xx.16` in §26; they
get real numbers when they are appended to DECISIONS.md. An engineer implementing `libs/bongfs`
(M7.6: format, allocation, inodes, extents, directories, journal, `mkfs.bongfs`, `fsck.bongfs`,
`bongfs-cp`, the fuzzer and the crash tester) and the kernel driver (M7.7) should be able to work
from this document alone.

**Contents**
1. Conventions
2. Checksums
3. Geometry and on-disk layout
4. Superblock
5. Feature flags and mount compatibility
6. Group descriptors and bitmaps
7. Inodes
8. Extents and the extent tree
9. Extended attributes
10. Directories
11. Symlinks, device nodes, FIFOs, sockets
12. Hard links and the orphan list
13. Allocation policy
14. Free-space accounting and reserved blocks
15. Journal: on-disk format
16. Journal: transactions, commit, checkpoint
17. Journal: replay
18. Mount, unmount, and runtime errors
19. mkfs.bongfs and bongfs-cp
20. Limits
21. Crash-consistency invariants
22. fsck.bongfs
23. Memory, portability, and `HOSTED` rules
24. Kernel driver notes (M7.7)
25. Test hooks for M7.6
26. Proposed DECISIONS.md rows
27. Deviations from ARCHITECTURE §15.1
28. Open questions for the owner

---

## 1. Conventions

- **Byte order.** Every multi-byte integer on disk is **little-endian**. Code decodes and encodes
  with byte-wise helpers (`le16Get(p)`, `le64Put(p, v)`, ...). It never casts a `uint8_t *` to a
  struct pointer: several structures below have unaligned fields (directory entries, xattr
  entries), and the same code runs on the host.
- **Types.** `u8/u16/u32/u64` are unsigned, `s64` is two's-complement signed. `u8[N]` is a byte
  array. Offsets and sizes in layout tables are in bytes, decimal.
- **Blocks.** A *block* is 4096 bytes. A *block number* is a `u64` counted from the start of the
  filesystem (the partition or image file), so block `b` is at byte offset `b * 4096`. The
  underlying `BlockDevice` (ARCHITECTURE §15) may have 512- or 4096-byte logical sectors; any
  other sector size is refused at mount (`STATUS_ERR_UNSUPPORTED`).
- **Reserved.** A field or byte range named `reserved` is written as zero. Readers do not
  interpret it (so a later version can define it under a feature flag). `fsck.bongfs` reports a
  non-zero reserved field and, in a repair mode, zeroes it (§22), unless an unknown feature bit is
  set.
- **MUST / MUST NOT / SHOULD / MAY** are used in the RFC 2119 sense. Every "MUST" in §7-§18 is
  something `fsck.bongfs` or the M7.6 tests check.
- **Integer math only.** Every formula in this document is integer arithmetic. `a / b` is
  truncating division of non-negative integers, `ceil(a / b)` is `(a + b - 1) / b`.
- **Names.** The C API uses the prefix `bfs` (`bfsMount`, `BfsSuperblock`, `BFS_BLOCK_SIZE`),
  never the OS name (ARCHITECTURE §4, D-046). The magic numbers below also avoid the OS name.
- **Status codes.** Functions return `Status` (ARCHITECTURE §4). This spec uses these names:
  `STATUS_OK`, `STATUS_ERR_INVALID`, `STATUS_ERR_NOT_FOUND`, `STATUS_ERR_NO_MEMORY`,
  `STATUS_ERR_UNSUPPORTED` (these exist today), and `STATUS_ERR_CORRUPT`, `STATUS_ERR_IO`,
  `STATUS_ERR_NO_SPACE`, `STATUS_ERR_READ_ONLY`, `STATUS_ERR_EXISTS`, `STATUS_ERR_NOT_EMPTY`,
  `STATUS_ERR_NAME_TOO_LONG`, `STATUS_ERR_FILE_TOO_BIG`, `STATUS_ERR_TOO_MANY_LINKS`,
  `STATUS_ERR_NOT_DIR`, `STATUS_ERR_IS_DIR`, `STATUS_ERR_ACCESS_DENIED` (to be added to
  `kernel/include/uapi/status.h`; see Open question Q6).

## 2. Checksums

### 2.1 CRC32C

bongfs uses CRC32C (Castagnoli): reflected polynomial `0x82F63B78`, initial value `0xFFFFFFFF`,
final XOR `0xFFFFFFFF`, input and output reflected (the standard iSCSI/SCTP/ext4 CRC32C).

Define two functions:
- `crc32cRaw(state, bytes)`: the reflected byte-wise update, with no initial value or final XOR.
  Table form: `state = table[(state ^ b) & 0xFF] ^ (state >> 8)` for each byte `b`, where
  `table[i]` is `i` shifted right 8 times, XORing `0x82F63B78` after each shift that drops a 1 bit.
- `CRC32C(bytes) = crc32cRaw(0xFFFFFFFF, bytes) XOR 0xFFFFFFFF`.

Known answers (host tests MUST check all of them):

| Input | CRC32C |
|---|---|
| ASCII `123456789` | `0xE3069283` |
| 32 zero bytes | `0x8A9136AA` |
| the 16 bytes `00 01 02 ... 0F` | `0xD9C908EB` |

An implementation MAY use the SSE4.2 `crc32` instruction in the kernel (it computes the same
function), but the portable table version is the reference and is what `libs/bongfs` ships.

### 2.2 The filesystem seed and `metaCsum`

Every checksum is seeded with the filesystem UUID (superblock `uuid`, 16 bytes), so a block left
over from an earlier filesystem on the same disk never validates.

- `seedState = crc32cRaw(0xFFFFFFFF, uuid[0..15])`, i.e. the CRC32C of the UUID without its final
  XOR. Compute it once at mount.
- `metaCsum(parts...) = crc32cRaw(seedState, parts concatenated) XOR 0xFFFFFFFF`, which equals
  `CRC32C(uuid || parts...)`.

Worked example: with `uuid = 00 01 02 ... 0F`, `seedState = 0x2636F714`, and
`metaCsum("123456789") = 0x699D042F`.

### 2.3 Coverage

The checksum field is treated as **zero** while computing (write it as 0, compute, then store the
result). `le32(x)`/`le64(x)` below mean the 4/8 little-endian bytes of `x`. Checksums are computed
when the structure is written to disk (for journaled metadata: when the transaction's frozen copy
is made, §16.4 step C4), never on every in-memory modification.

| Structure | Checksum field | Computed as |
|---|---|---|
| Superblock (primary and backups) | `csum` (offset 508) | `metaCsum(sb[0..511])` |
| Group descriptor for group `g` | `csum` (offset 60) | `metaCsum(le32(g), gd[0..63])` |
| Block bitmap of group `g` | GD `blockBitmapCsum` | `metaCsum(le32(g), bitmap[0..4095])` |
| Inode bitmap of group `g` | GD `inodeBitmapCsum` | `metaCsum(le32(g), bitmap[0..4095])` |
| Inode number `ino` (the whole 256-byte slot, including the inline extent root, inline directory, inline symlink, and inline xattrs) | `csum` (offset 92) | `metaCsum(le64(ino), inode[0..255])` |
| Extent tree block | header `csum` (offset 12) | `metaCsum(block[0..4095])` |
| Directory block (hashed directories) | header `csum` (offset 44) | `metaCsum(block[0..4095])` |
| Xattr overflow block | header `csum` (offset 28) | `metaCsum(block[0..4095])` |
| Journal superblock slot | `csum` (offset 20) | `metaCsum(jsb[0..511])` |
| Journal descriptor block | `csum` (offset 20) | `metaCsum(block[0..4095])` |
| Journal commit block | `csum` (offset 20) | `metaCsum(block[0..4095])` |
| Logged block copy in the journal | tag `dataCsum` | `metaCsum(le64(seq), le64(targetBlock), copy[0..4095])` |

Not checksummed: block 0; file data blocks (including the data block of a long symlink); the
unused tail bytes 512..4095 of the superblock block and of each journal-superblock block (they
MUST be zero; fsck checks); inode-table slots at or above the group's `inodesInitialized`
(§6.1); unused group-descriptor slots after the last group (MUST be zero; fsck checks).

Binding: extent, directory, and xattr blocks record their owner inode number and generation (and
extent/xattr blocks their own block number, directory blocks their logical index) inside the
checksummed header, so a misdirected or stale block fails validation even though its checksum
is internally consistent. Readers MUST check these binding fields as well as the checksum.

---

## 3. Geometry and on-disk layout

### 3.1 Constants

| Name | Value |
|---|---|
| `BFS_BLOCK_SIZE` | 4096 (`blockSizeLog2` = 12) |
| `BFS_BLOCKS_PER_GROUP` | 32768 (128 MiB; one bitmap block covers exactly one group) |
| `BFS_INODE_SIZE` | 256 (16 inodes per inode-table block) |
| `BFS_GD_SIZE` | 64 (64 descriptors per GDT block) |
| `BFS_ROOT_INO` | 2 |
| `BFS_FIRST_INO` | 16 (first inode number available to files) |
| `BFS_MAX_GROUPS` | 524288 (2^19) |

### 3.2 Groups

Group `g` (0 <= g < `groupCount`) covers blocks `[g * 32768, min((g + 1) * 32768, blockCount))`.
`groupCount = ceil(blockCount / 32768)`. The last group may be partial.

A group **has a superblock copy** (`hasSb(g)`) when `g == 0`, `g == 1`, or `g` is a power of 3,
5, or 7 (3^k, 5^k, 7^k with k >= 1): 0, 1, 3, 5, 7, 9, 25, 27, 49, 81, 125, 243, 343, 625, 729,
... Group 0 holds the **primary** superblock; the others hold **backups**.

Layout of each group, in block order (`base = g * 32768`, `gdtBlocks = ceil(groupCount / 64)`,
`itableBlocks = inodesPerGroup / 16`):

| Group kind | Blocks, in order |
|---|---|
| `g == 0` | block 0 (boot area), block 1 (primary superblock), blocks `2 .. 1 + gdtBlocks` (group descriptor table, GDT), block bitmap, inode bitmap, inode table (`itableBlocks`), **journal** (`journalBlocks`), data |
| `g > 0` and `hasSb(g)` | `base` (backup superblock), block bitmap, inode bitmap, inode table, data |
| other `g` | `base` (block bitmap), inode bitmap, inode table, data |

Derived positions (these MUST equal the group descriptor fields; v1 has no flexible placement):

```
bbStart(g)   = (g == 0) ? 2 + gdtBlocks : base + (hasSb(g) ? 1 : 0)
ibStart(g)   = bbStart(g) + 1
itStart(g)   = bbStart(g) + 2
journalStart = itStart(0) + itableBlocks
firstData(g) = itStart(g) + itableBlocks + (g == 0 ? journalBlocks : 0)
groupEnd(g)  = min(base + 32768, blockCount)
```

The **data area** of group `g` is `[firstData(g), groupEnd(g))`. Every group MUST have at least
one data block (`firstData(g) < groupEnd(g)`). Every block outside all data areas is fixed
metadata (block 0, superblocks, GDT, bitmaps, inode tables, journal) and is always marked used.

There are no GDT backups. Every GDT field except the counters and bitmap checksums is derivable
from the superblock geometry, and `fsck.bongfs` rebuilds the counters from the bitmaps (§22).

### 3.3 Block 0

Block 0 (the first 4 KiB of the partition) is the **boot area**. bongfs never reads or writes it
after mkfs. `mkfs.bongfs` writes it as 4096 zero bytes; this also destroys the signatures of any
earlier filesystem that lived there (for example an ext4 superblock at byte 1024 or a FAT boot
sector), so probing tools don't misidentify the partition. It is reserved for a future boot
record and is marked used in group 0's block bitmap.

### 3.4 Sizing rules (mkfs defaults; §19 has the options)

1. `blockCount = deviceBytes / 4096`, capped at `2^34` (64 TiB, `BFS_MAX_GROUPS` groups).
2. If `blockCount < 4096` (16 MiB): refuse (`STATUS_ERR_INVALID`).
3. Inode ratio `R` (bytes per inode, default 16384, allowed 4096..1048576, a power of two):
   `inodesPerGroup = ((min(32768, blockCount) * 4096 / R) / 16) * 16`, then clamped to
   `[64, 32768]`.
4. Journal: `journalBlocks = clamp(((blockCount / 64) / 256) * 256, 1024, 16384)`, unless `-J`
   gives a value in `[1024, 16384]`.
5. Compute the layout. If the last group has fewer than 1024 data blocks and `groupCount > 1`,
   drop it: `blockCount = (groupCount - 1) * 32768`, recompute from step 3. If group 0 then has
   fewer than 1024 data blocks, refuse (`STATUS_ERR_INVALID`).
6. `reservedBlocks = min(blockCount * m / 100, 262144)` with `m` = the `-m` percentage (default
   5, allowed 0..50).

The validity rules the mount checks (§4.2) are looser than these defaults: any geometry that
satisfies them mounts.

### 3.5 Worked examples (default options)

All numbers computed with the rules above (the script is in the M7.5 log).

**16 MiB (the minimum):** `blockCount` 4096, 1 group, `gdtBlocks` 1, `inodesPerGroup` 1024
(64 inode-table blocks), `inodeCount` 1024, `journalBlocks` 1024 (4 MiB), no backups.
Block 0 boot, 1 superblock, 2 GDT, 3 block bitmap, 4 inode bitmap, 5..68 inode table, 69..1092
journal, first data block 1093. Free blocks after mkfs: 3003. `reservedBlocks` 204.

**1 GiB:** `blockCount` 262144, 8 groups, `gdtBlocks` 1, `inodesPerGroup` 8192 (512 blocks),
`inodeCount` 65536, `journalBlocks` 4096 (16 MiB), backups in groups 1, 3, 5, 7.

| Group | Base | Superblock | Block bitmap | Inode bitmap | Inode table | Journal | First data | Data blocks |
|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 1 (GDT at 2) | 3 | 4 | 5..516 | 517..4612 | 4613 | 28155 |
| 1 | 32768 | 32768 | 32769 | 32770 | 32771..33282 | - | 33283 | 32253 |
| 2 | 65536 | - | 65536 | 65537 | 65538..66049 | - | 66050 | 32254 |
| 3 | 98304 | 98304 | 98305 | 98306 | 98307..98818 | - | 98819 | 32253 |
| 7 | 229376 | 229376 | 229377 | 229378 | 229379..229890 | - | 229891 | 32253 |

Groups 4 and 6 look like group 2, group 5 like group 3. Total free blocks after mkfs:
`28155 + 4 * 32253 + 3 * 32254 = 253929` (the root directory and `lost+found` are inline and use
no blocks). Free inodes: `65536 - 16 = 65520` (inodes 1..15 reserved, 16 is `lost+found`).
`reservedBlocks` = 13107.

**1000 MiB (partial last group):** `blockCount` 256000, 8 groups, the last group (7, a backup
group) has 26624 blocks and 26109 data blocks. `journalBlocks` = `((256000 / 64) / 256) * 256` =
3840, so the first data block of group 0 is 4357.

**1 TiB:** `blockCount` 268435456, 8192 groups, `gdtBlocks` 128 (blocks 2..129), block bitmap
130, inode bitmap 131, inode table 132..643, journal 644..17027 (16384 blocks, 64 MiB), first
data block 17028. `inodeCount` 67108864. 18 backups: groups 1, 3, 5, 7, 9, 25, 27, 49, 81, 125,
243, 343, 625, 729, 2187, 2401, 3125, 6561. `reservedBlocks` 262144 (the 1 GiB cap).

**64 TiB (the maximum):** 524288 groups, `gdtBlocks` 8192 (blocks 2..8193), inode table
8196..8707, journal 8708..25091, first data block 25092 (7676 data blocks in group 0). With
`-i 4096` (32768 inodes per group, 2048 inode-table blocks) the first data block is 26628: group 0
still fits, which is why `BFS_MAX_GROUPS` is 2^19. `inodeCount` at the default ratio is 2^32, so
inode numbers are 64-bit on disk.

---

## 4. Superblock

### 4.1 Placement and layout

The primary superblock is the first 512 bytes of **block 1** (byte offset 4096). A backup is the
first 512 bytes of the first block of each group `g > 0` with `hasSb(g)`. Every field and the
checksum sit in the first 512 bytes on purpose: the superblock is sometimes written directly
(not through the journal, §18), and on a 512-byte-sector device a torn 4 KiB write then either
lands the whole first sector or none of it (bytes 512..4095 are zero before and after).

**Superblock block:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 512 | Superblock | sb | the superblock structure below |
| 512 | 3584 | u8[3584] | reserved | zero; not checksummed; fsck checks it is zero |

Total size: 4096 bytes

**Superblock structure** (D = dynamic field, see §4.3):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | magic | `0x0A1A0A0D5346427F` (bytes `7F 42 46 53 0D 0A 1A 0A`) |
| 8 | 2 | u16 | versionMajor | 1. Any other value: not mountable (`STATUS_ERR_UNSUPPORTED`) |
| 10 | 2 | u16 | versionMinor | 0 when written by v1. Informational; readers accept any value (feature bits carry the semantics) |
| 12 | 4 | u32 | blockSizeLog2 | 12 (4096-byte blocks). Any other value: `STATUS_ERR_UNSUPPORTED` |
| 16 | 8 | u64 | blockCount | total blocks in the filesystem, block 0 included; 4096..2^34; the device MUST hold at least `blockCount * 4096` bytes |
| 24 | 8 | u64 | inodeCount | MUST equal `groupCount * inodesPerGroup` |
| 32 | 4 | u32 | blocksPerGroup | 32768 |
| 36 | 4 | u32 | inodesPerGroup | 64..32768, a multiple of 16 |
| 40 | 4 | u32 | groupCount | MUST equal `ceil(blockCount / 32768)`; 1..524288 |
| 44 | 4 | u32 | gdtBlocks | MUST equal `ceil(groupCount / 64)` |
| 48 | 2 | u16 | inodeSize | 256 |
| 50 | 2 | u16 | groupDescSize | 64 |
| 52 | 4 | u32 | sbGroup | the group this copy lives in: 0 for the primary, `g` for a backup |
| 56 | 8 | u64 | featureCompat | §5 |
| 64 | 8 | u64 | featureIncompat | §5 (bit 0 `NEEDS_RECOVERY` is dynamic) |
| 72 | 8 | u64 | featureRoCompat | §5 |
| 80 | 16 | u8[16] | uuid | filesystem UUID (RFC 4122 v4 from mkfs); all-zero is invalid; seeds every checksum (§2.2) |
| 96 | 16 | u8[16] | dirHashKey | SipHash-2-4 key for directory hashing (§10.2); random from mkfs; any value valid |
| 112 | 64 | u8[64] | label | UTF-8 volume label, 0..63 bytes, NUL-padded; byte 63 MUST be 0 and no NUL may precede a non-NUL byte |
| 176 | 8 | u64 | journalStart | MUST equal the derived `journalStart` (§3.2) |
| 184 | 4 | u32 | journalBlocks | 1024..16384 |
| 188 | 4 | u32 | firstInode | 16 |
| 192 | 8 | u64 | rootInode | 2 |
| 200 | 8 | u64 | orphanHead | D. First inode of the orphan list (§12.2), or 0 |
| 208 | 8 | u64 | reservedBlocks | blocks only privileged allocations may use (§14.2); MUST be <= `blockCount / 2` |
| 216 | 4 | u32 | reservedUid | uid allowed to use reserved blocks (mkfs: 0) |
| 220 | 4 | u32 | reservedGid | gid allowed to use reserved blocks (mkfs: 0) |
| 224 | 8 | u64 | freeBlocksHint | D. Free blocks at the last clean unmount; not authoritative (§14.1) |
| 232 | 8 | u64 | freeInodesHint | D. Free inodes at the last clean unmount; not authoritative |
| 240 | 4 | u32 | stateFlags | D. Bit 0 `ERRORS`: corruption was detected and fsck hasn't cleared it. Other bits 0 |
| 244 | 2 | u16 | errorBehavior | on runtime corruption: 1 = remount read-only (mkfs default), 2 = panic (kernel; host tools treat it as 1). Other values are treated as 1 |
| 246 | 2 | u16 | reserved | 0 |
| 248 | 4 | u32 | mountCount | D. Read-write mounts since mkfs (wraps) |
| 252 | 4 | u32 | reserved | 0 |
| 256 | 8 | s64 | createTimeNs | mkfs time (ns since the Unix epoch, §7.5) |
| 264 | 8 | s64 | lastMountTimeNs | D. Last read-write mount |
| 272 | 8 | s64 | lastWriteTimeNs | D. Last clean unmount |
| 280 | 8 | s64 | lastCheckTimeNs | D. Last fsck that left the filesystem clean |
| 288 | 220 | u8[220] | reserved | 0 |
| 508 | 4 | u32 | csum | `metaCsum(sb[0..511])` with this field as zero |

Total size: 512 bytes

### 4.2 Superblock validation (mount and fsck, in this order)

1. `magic` matches, else "not a bongfs filesystem": `STATUS_ERR_NOT_FOUND`.
2. `versionMajor == 1` and `blockSizeLog2 == 12`, else `STATUS_ERR_UNSUPPORTED`.
3. `csum` matches, else `STATUS_ERR_CORRUPT`.
4. Feature bits (§5): an unknown `featureIncompat` bit gives `STATUS_ERR_UNSUPPORTED`.
5. Geometry: every "MUST"/range rule in the table above holds; `sbGroup == 0` for the primary;
   `groupCount <= 524288`; every group has at least one data block (§3.2); the device is at least
   `blockCount * 4096` bytes. Any failure: `STATUS_ERR_CORRUPT`.

The kernel never mounts from a backup. `fsck.bongfs` uses backups (§22, check F0.1).

### 4.3 Static and dynamic fields, backups

**Dynamic fields** (marked D) change during normal operation: `orphanHead`, `freeBlocksHint`,
`freeInodesHint`, `stateFlags`, `mountCount`, `lastMountTimeNs`, `lastWriteTimeNs`,
`lastCheckTimeNs`, and bit 0 (`NEEDS_RECOVERY`) of `featureIncompat`. Every other field is
**static**: v1 only changes it in `mkfs.bongfs` and in `fsck.bongfs` repairs.

A **backup** superblock is the primary with `sbGroup = g`, every dynamic field (and the
`NEEDS_RECOVERY` bit) set to 0, and `csum` recomputed. `mkfs.bongfs` writes all backups. Whoever
changes a static field (only fsck in v1) rewrites every backup afterwards. The kernel driver never
writes backups.

---

## 5. Feature flags and mount compatibility

### 5.1 Rules

| Set | A reader that doesn't know a set bit | Writers |
|---|---|---|
| `featureCompat` | ignores it; may mount read-write | preserve unknown bits |
| `featureRoCompat` | may mount read-only only; replay is still allowed (§17) | never write |
| `featureIncompat` | must not mount at all, and must not replay the journal | never write |

`fsck.bongfs` refuses to **repair** a filesystem with any unknown bit in any of the three sets
(exit 8). It may still check it with `-n` when only compat/roCompat bits are unknown.

Any change to the journal format (§15) in a later version MUST be an incompat feature, which is
what makes replay safe under unknown roCompat bits.

### 5.2 v1 bits

| Set | Bit | Name | Meaning |
|---|---|---|---|
| incompat | 0 | `NEEDS_RECOVERY` | dynamic. Set while mounted read-write (and left set by a crash): the journal may hold committed transactions. Cleared by clean unmount and after replay by a read-only mount (§18). A reader that can't replay must refuse the filesystem while it is set |
| incompat | 1 | `SNAPSHOTS` | **reserved** for M20.1 (refcounted extents, copy-on-write). v1 never sets it and, not implementing it, refuses to mount when it is set |
| incompat | 2..63 | - | unassigned, 0 |
| roCompat | 0..63 | - | unassigned, 0 |
| compat | 0..63 | - | unassigned, 0 |

TRIM/discard needs no feature bit: it changes no on-disk structure. It is a mount option (§13.6).

### 5.3 Mount decision table

"rw" is a read-write mount request, "ro" read-only. `ERRORS` is superblock `stateFlags` bit 0.

| Condition | rw mount | ro mount | Notes |
|---|---|---|---|
| `magic` wrong | `STATUS_ERR_NOT_FOUND` | same | not bongfs |
| `versionMajor` / `blockSizeLog2` wrong | `STATUS_ERR_UNSUPPORTED` | same | |
| unknown incompat bit (includes `SNAPSHOTS`) | `STATUS_ERR_UNSUPPORTED` | same | no replay either |
| unknown roCompat bit | `STATUS_ERR_UNSUPPORTED` | allowed | the boot-time root mount retries read-only and prints a warning |
| unknown compat bit | allowed | allowed | bit preserved |
| primary superblock checksum or geometry bad | `STATUS_ERR_CORRUPT` | same | run `fsck.bongfs` (uses backups) |
| a group descriptor's checksum or derived fields bad | `STATUS_ERR_CORRUPT` | allowed | ro derives bitmap/table locations from geometry and never trusts that group's counters |
| both journal superblock slots invalid (§15.2) | `STATUS_ERR_CORRUPT` | allowed only if `NEEDS_RECOVERY` is clear | otherwise the committed state is unknowable |
| journal needs replay and the device is write-protected | `STATUS_ERR_READ_ONLY` | same | host `fsck.bongfs -n` can still check it (in-memory replay) |
| `ERRORS` set (including set by this mount's replay, §17 step R5) | `STATUS_ERR_READ_ONLY` | allowed, with a warning | cleared only by a clean fsck (see Q3) |
| a metadata checksum fails later, at runtime | - | - | the operation fails with `STATUS_ERR_CORRUPT`, then the error handler runs (§18.3) |

---

## 6. Group descriptors and bitmaps

### 6.1 Group descriptor

The GDT occupies blocks `2 .. 1 + gdtBlocks`. Descriptor `g` is at byte offset `(g % 64) * 64`
of GDT block `2 + g / 64`. Slots after the last group in the last GDT block are all zero.

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | blockBitmap | MUST equal `bbStart(g)` |
| 8 | 8 | u64 | inodeBitmap | MUST equal `ibStart(g)` |
| 16 | 8 | u64 | inodeTable | MUST equal `itStart(g)` |
| 24 | 4 | u32 | freeBlocks | MUST equal the number of 0 bits in this group's block bitmap |
| 28 | 4 | u32 | freeInodes | MUST equal the number of 0 bits among the first `inodesPerGroup` bits of the inode bitmap |
| 32 | 4 | u32 | usedDirs | MUST equal the number of allocated directory inodes in this group |
| 36 | 4 | u32 | inodesInitialized | high-water mark: every slot at index `>= inodesInitialized` has never been allocated since mkfs, its bitmap bit is 0 and its contents are undefined. 0..`inodesPerGroup` |
| 40 | 4 | u32 | blockBitmapCsum | `metaCsum(le32(g), block bitmap)` |
| 44 | 4 | u32 | inodeBitmapCsum | `metaCsum(le32(g), inode bitmap)` |
| 48 | 12 | u8[12] | reserved | 0 |
| 60 | 4 | u32 | csum | `metaCsum(le32(g), gd[0..63])` with this field as zero |

Total size: 64 bytes

The GDT is journaled metadata. Whenever a bitmap block changes in a transaction, its group's
descriptor (counters and bitmap checksum) changes in the same transaction.

### 6.2 Bitmaps

Both bitmaps are one full block. Bit `i` is bit `i % 8` (LSB first) of byte `i / 8`. 1 = used.

- **Block bitmap of group `g`:** bit `i` describes block `g * 32768 + i`. Bits for blocks at or
  past `blockCount` (the tail of a partial last group) are 1. All fixed-metadata blocks of the
  group (§3.2) are 1.
- **Inode bitmap of group `g`:** bit `i` (`i < inodesPerGroup`) describes inode
  `g * inodesPerGroup + i + 1`. Bits `inodesPerGroup .. 32767` are 1 (padding). In group 0, bits
  0..14 (inodes 1..15, the reserved inodes) are always 1.

The bitmaps are authoritative for allocation; every counter is derived from them (§14).

---

## 7. Inodes

### 7.1 Numbering

Inode numbers are `u64`, starting at 1; 0 means "none". Inode `ino` (1 <= ino <= `inodeCount`)
lives at:

```
group  = (ino - 1) / inodesPerGroup
index  = (ino - 1) % inodesPerGroup
block  = itStart(group) + index / 16
offset = (index % 16) * 256
```

| Inode | Use |
|---|---|
| 1 | reserved, never used |
| 2 | root directory |
| 3..15 | reserved for future features (M20.1 may use one for the snapshot table, under its incompat bit) |
| 16 | `lost+found` as created by mkfs (fsck may recreate it under another number) |
| 16.. | ordinary files |

Reserved inodes 1 and 3..15 are in the **free form** (§7.7) with generation 0; their bitmap bits
are 1. v1 never changes them.

### 7.2 Layout

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 2 | u16 | mode | file type (bits 12..15) and permissions (bits 0..11), §7.3 |
| 2 | 2 | u16 | xattrInlineUsed | bytes used in `xattrInline`, 0..48 (§9) |
| 4 | 4 | u32 | flags | §7.4 |
| 8 | 4 | u32 | uid | owner. Any value; writers MUST NOT store `0xFFFFFFFF` (fsck warns) |
| 12 | 4 | u32 | gid | group. Same rule as `uid` |
| 16 | 4 | u32 | linkCount | directory entries naming this inode (directories: 2 + subdirectories), §12.1. 1..`0x7FFFFFFF`, or 0 only while on the orphan list |
| 20 | 4 | u32 | generation | incremented each time the slot is allocated (§7.6); any value |
| 24 | 8 | u64 | size | bytes; per-type rules in §7.8 |
| 32 | 8 | u64 | allocatedBlocks | 4 KiB blocks charged to the inode: mapped data blocks (written and unwritten) + extent tree blocks (not the inline root) + the xattr block. `st_blocks = allocatedBlocks * 8` |
| 40 | 8 | s64 | atimeNs | last access (§7.5) |
| 48 | 8 | s64 | mtimeNs | last data modification |
| 56 | 8 | s64 | ctimeNs | last inode change |
| 64 | 8 | s64 | btimeNs | creation (birth) time; set once at allocation |
| 72 | 8 | u64 | xattrBlock | xattr overflow block (§9.3), or 0 |
| 80 | 8 | u64 | orphanNext | next inode on the orphan list, or 0 (§12.2). MUST be 0 unless the `ORPHAN` flag is set |
| 88 | 4 | u32 | reserved | 0 |
| 92 | 4 | u32 | csum | `metaCsum(le64(ino), inode[0..255])` with this field as zero |
| 96 | 112 | u8[112] | iData | type-dependent: extent root (§8.2), inline directory (§10.4), inline symlink target, or device number (§11) |
| 208 | 48 | u8[48] | xattrInline | inline extended attributes (§9.2); bytes past `xattrInlineUsed` are 0 |

Total size: 256 bytes

### 7.3 Mode

POSIX values. `mode & 0xF000` is the type:

| Type bits | Type | `fileType` in directory entries |
|---|---|---|
| `0x8000` | regular file | 1 |
| `0x4000` | directory | 2 |
| `0x2000` | character device | 3 |
| `0x6000` | block device | 4 |
| `0x1000` | FIFO | 5 |
| `0xC000` | socket | 6 |
| `0xA000` | symbolic link | 7 |

Any other type value in an allocated inode is corruption. `mode & 0x0FFF` holds the permission
bits including setuid (`0x800`), setgid (`0x400`) and sticky (`0x200`). bongfs stores them;
whether the kernel honors setuid/setgid is a VFS policy (Q4). Symlinks are always written with
permissions `0777` (ignored). `fileType` 0 is invalid.

### 7.4 Flags

| Bit | Name | Meaning |
|---|---|---|
| 0 | `INLINE` | `iData` holds an inline directory (§10.4) or an inline symlink target (§11.1) instead of an extent root. Set on directories and symlinks only; never on regular files in v1 |
| 1 | `IMMUTABLE` | no modification, link, unlink, or rename of this inode (enforced by the driver; only uid 0 changes the flag) |
| 2 | `APPEND` | writes only at end of file; no truncate-down (driver-enforced, as above) |
| 3 | `NOATIME` | the driver never updates `atimeNs` |
| 4 | `ORPHAN` | the inode is on the orphan list (§12.2) |
| 5 | `TRUNCATING` | a multi-transaction truncate is in progress; only valid together with `ORPHAN` |
| 6..31 | - | 0. A later version defines new bits only under a feature flag, so an unknown set bit is corruption |

### 7.5 Timestamps

`s64` nanoseconds since 1970-01-01T00:00:00 UTC, POSIX time (no leap seconds). Negative values
are before 1970. Range: years 1677..2262. There is no separate seconds field.

- Create: `atime = mtime = ctime = btime = now`; the parent directory's `mtime = ctime = now`.
- Data write or truncate: `mtime = ctime = now`.
- Any inode change (chmod, chown, link, unlink, rename, xattr change): `ctime = now`.
- `atime` follows the mount option (default **relatime**: update only if `atime <= mtime`,
  `atime <= ctime`, or `atime` is more than 24 h old). `atime`-only updates MAY be delayed and
  batched into a later transaction, and are not covered by the crash invariants (§21).

### 7.6 Generation

When a slot is allocated: if its index is below the group's `inodesInitialized`, read the old
slot and set `generation = old.generation + 1` (mod 2^32; if the old slot fails its checksum, use
1 and log a warning); otherwise `generation = 1`. Extent, directory, and xattr blocks record the
owner's generation (§2.3), so a block that survives from a previous incarnation of the inode
number never validates.

### 7.7 Free form

An unallocated slot below `inodesInitialized` is in the **free form**: every byte 0 except
`generation` (kept, so the next allocation can increment it) and `csum` (valid). Freeing an inode
writes the free form in the same transaction that clears its bitmap bit. Slots at or above
`inodesInitialized` are undefined and never read.

Allocation (§13.2) always takes the **lowest-numbered free slot** of the chosen group. If that
index is `>= inodesInitialized`, it is exactly `inodesInitialized` (all lower slots are in use),
and the transaction sets `inodesInitialized = index + 1`.

### 7.8 Per-type validity rules

| Type | `INLINE` | `iData` | `size` | `allocatedBlocks` (x = 1 if `xattrBlock != 0`, else 0) |
|---|---|---|---|---|
| regular | 0 | extent root (§8.2) | 0..2^44 | mapped blocks + tree blocks + x |
| directory, inline | 1 | inline directory (§10.4) | 112 | x |
| directory, hashed | 0 | extent root mapping logical blocks `0 .. size/4096 - 1`, all written, no holes | a multiple of 4096, >= 4096 | `size/4096` + tree blocks + x |
| symlink, short (1..112 bytes) | 1 | target bytes, then zeros | target length, 1..112 | x |
| symlink, long (113..4095 bytes) | 0 | extent root with exactly one written extent: logical 0, length 1 | target length, 113..4095 | 1 + x |
| char / block device | 0 | `u32 major`, `u32 minor`, then 104 zero bytes | 0 | x |
| FIFO, socket | 0 | 112 zero bytes | 0 | x |

Also, for every allocated inode: `linkCount >= 1` unless `ORPHAN` is set (a removed directory
that is still open has 0); other directories have `linkCount >= 2`; `TRUNCATING` implies
`ORPHAN`; bytes of `xattrInline` past `xattrInlineUsed` are 0.

---

## 8. Extents and the extent tree

### 8.1 Model

A file's logical block `L` (byte range `[L * 4096, (L + 1) * 4096)`) maps through a B+tree of
extents keyed by logical block. Logical block numbers are `u32`, so a file has at most 2^32
blocks (16 TiB). An unmapped logical block is a **hole** and reads as zeros. An **unwritten**
extent is allocated but reads as zeros until written (preallocation, `fallocate`).

The tree root lives in the inode's `iData`. Its header has the same first 8 bytes as a tree-block
header, so one parser serves both.

### 8.2 Root (in `iData`)

**Root header:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 2 | u16 | magic | `0x5458` (bytes `58 54`, "XT") |
| 2 | 2 | u16 | entries | 0..6 |
| 4 | 2 | u16 | maxEntries | 6 |
| 6 | 2 | u16 | depth | 0 = the root holds leaf extents; 1..4 = the root holds index entries. At most 4 |
| 8 | 8 | u64 | reserved | 0 |

Total size: 16 bytes

**Root (the whole `iData`):**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 16 | ExtentRootHeader | header | above |
| 16 | 96 | u8[96] | entries | 6 slots of 16 bytes: leaf extents (depth 0) or index entries (depth > 0); unused slots are 0 |

Total size: 112 bytes

An empty file has `entries = 0`, `depth = 0`.

### 8.3 Tree blocks

**Tree-block header:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 2 | u16 | magic | `0x5458` |
| 2 | 2 | u16 | entries | 1..254 (a tree block is never empty) |
| 4 | 2 | u16 | maxEntries | 254 |
| 6 | 2 | u16 | depth | 0 = leaf; else this block's height above the leaves. MUST equal the parent's depth - 1 |
| 8 | 4 | u32 | ownerGeneration | owner inode's `generation` |
| 12 | 4 | u32 | csum | `metaCsum(block[0..4095])` with this field as zero |
| 16 | 8 | u64 | selfBlock | this block's own block number |
| 24 | 8 | u64 | ownerIno | owner inode number |

Total size: 32 bytes

**Tree block:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 32 | ExtentBlockHeader | header | above |
| 32 | 4064 | u8[4064] | entries | 254 slots of 16 bytes; unused slots are 0 |

Total size: 4096 bytes

**Leaf extent** (depth 0):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | u32 | logicalBlock | first logical block mapped |
| 4 | 2 | u16 | length | 1..32768 blocks; `logicalBlock + length <= 2^32` (compute in 64 bits) |
| 6 | 2 | u16 | flags | bit 0 `UNWRITTEN`; bits 1..15 are 0 |
| 8 | 8 | u64 | physicalBlock | first physical block; the whole run `[physicalBlock, physicalBlock + length)` lies inside one group's data area (§3.2) |

Total size: 16 bytes

**Index entry** (depth > 0):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | u32 | logicalBlock | separator key; stored as 0 in entry 0 of every index node (§8.4) |
| 4 | 4 | u32 | reserved | 0 |
| 8 | 8 | u64 | childBlock | block number of the child tree block, inside a data area |

Total size: 16 bytes

### 8.4 Tree invariants

1. **Ranges.** The root covers logical blocks `[0, 2^32)`. In an index node with entries
   `e[0..n-1]` covering `[lo, hi)`, child `i` covers `[e[i].logicalBlock, e[i+1].logicalBlock)`,
   reading `e[0].logicalBlock` as `lo` and `e[n].logicalBlock` as `hi`. `e[0].logicalBlock` is
   stored as 0 and ignored; keys `e[1..n-1]` are strictly increasing and inside `(lo, hi)`.
2. Leaf extents are sorted by `logicalBlock`, don't overlap (`e[i].logicalBlock + e[i].length <=
   e[i+1].logicalBlock`), and lie inside the leaf's range.
3. Depths are consistent (§8.3) and the root depth is at most 4. Depth 4 holds about 25 billion
   extents when every block is full and at least `127^4`, about 260 million, with half-full blocks,
   so only a pathologically fragmented file reaches the limit; an insert that would need depth 5
   fails with `STATUS_ERR_FILE_TOO_BIG`.
4. Every non-root node has at least one entry. Nodes have no minimum fill otherwise.
5. No physical block is mapped twice, within a file or across files (the only sharing feature,
   `SNAPSHOTS`, is incompat).
6. **EOF rule.** Let `eofBlocks = ceil(size / 4096)`. A **written** extent MUST satisfy
   `logicalBlock + length <= eofBlocks`, unless the inode has `TRUNCATING` set. Unwritten extents
   MAY extend past EOF (`fallocate` with keep-size).
7. Bytes of the last block past `size` (the partial tail) are **unspecified** on disk. Readers
   return zeros for them. Any operation that raises `size` from `S` MUST first make bytes
   `[S, min(newSize, ceil(S / 4096) * 4096))` zero on disk, when the block containing byte `S` is
   mapped and written, as an ordered data write (§16.5) of the transaction that raises `size`.

### 8.5 Lookup

Descend from the root: in an index node, take the last entry `i` whose key is `<= L` (entry 0
always qualifies); read the child block and verify it (magic, checksum, `selfBlock`, `ownerIno`,
`ownerGeneration`, `depth`, `entries`, `maxEntries`); repeat to depth 0. In the leaf, take the
last extent with `logicalBlock <= L`; `L` is mapped if `L < logicalBlock + length`, else it's a
hole. A path is at most 5 nodes (root + 4 blocks); implementations keep it in a fixed array.

### 8.6 Insert (map `[L, L + n)` to `[P, P + n)` with flags `f`; the range is currently a hole)

1. Split the request into pieces of at most 32768 blocks (each is a separate insert).
2. Descend to the leaf covering `L`. If the piece extends past that leaf's upper bound `hi`
   (§8.4 invariant 1), insert `[L, hi)` here and the rest as a separate insert, so every extent
   stays inside its leaf's range. Merges (step 3) likewise never extend an extent past `hi`.
3. **Merge:** if the previous extent in that leaf ends exactly at `L`, its physical run ends
   exactly at `P`, the flags are equal and the combined length is `<= 32768`, extend it. Likewise
   merge with the next extent in the same leaf. Both may merge (then the next extent is removed).
   Merging across leaves is not required.
4. Otherwise insert the new extent in sorted position. If the node is full:
   - **Root full (in-inode, 6 entries):** push down. Allocate one block `B`; copy the root's
     entries into `B` with `depth = rootDepth`; set the root to `depth = rootDepth + 1`, one entry
     `{0, B}`. If `rootDepth + 1 > 4`, fail with `STATUS_ERR_FILE_TOO_BIG` before changing
     anything. Continue the insert in `B`.
   - **Tree block full (254 entries):** split. Form the 255 sorted entries (the new one
     included); the left node keeps entries `[0, 127)`, a newly allocated right node gets
     `[127, 255)`. For a leaf, the separator is the right node's first `logicalBlock`. For an
     index node, the separator is the right node's first key, which is then stored as 0 in the
     right node. Insert `{separator, right}` into the parent after the left node's entry,
     splitting the parent the same way if it is full, up to the root.
5. Update `allocatedBlocks` for every new tree block.

All checks that can fail (depth limit, block allocation for every tree block the insert might
need: at most one per level plus the push-down) happen **before** the first modification, so a
failed insert changes nothing (§16.2).

### 8.7 Remove (unmap `[L, L + n)`)

1. For each extent overlapping the range: remove it entirely, trim its head or tail, or split it
   in two (a punch in the middle; the second half is an insert, §8.6). Freed physical blocks go
   to the deferred-free list (§13.5), not straight back to the bitmap.
2. A non-root node that becomes empty is freed and its entry removed from the parent. If that was
   the parent's entry 0, the new entry 0's key is set to 0 (invariant 1). Repeat upward.
3. If the root index becomes empty, reset it to `depth = 0, entries = 0`.
4. **Collapse:** while the root has `depth >= 1`, exactly one entry, and that child has at most 6
   entries: copy the child's entries into the root, set the root depth to the child's depth, and
   free the child.
5. Update `allocatedBlocks`.

### 8.8 Unwritten extents

Written data inside an unwritten extent converts that part to written: the extent splits into at
most three (unwritten head, written middle, unwritten tail), which is at most two inserts. The
conversion is metadata in the transaction that commits after the data write completes (§16.5).
A read of an unwritten extent returns zeros without touching the disk.

---

## 9. Extended attributes

### 9.1 Names and entries

An attribute is `(namespace, name, value)`. Namespaces: 1 = `user.`, 2 = `trusted.`,
3 = `security.`, 4 = `system.` (the VFS maps the textual prefix; the prefix is not stored). Other
namespace values are corruption. `name` is 1..255 bytes with no NUL; `value` is 0..N bytes (N
below). `(namespace, name)` is unique per inode across both storage areas.

**Xattr entry** (fixed part; followed by `nameLen` name bytes and then `valueLen` value bytes,
no padding, no terminator):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 1 | u8 | namespace | 1..4 |
| 1 | 1 | u8 | nameLen | 1..255 |
| 2 | 2 | u16 | valueLen | value length in bytes |

Total size: 4 bytes

Within each area entries are sorted by `namespace`, then by name bytes (unsigned byte-wise
comparison; a proper prefix sorts first), and packed with no gaps.

### 9.2 Inline area

`xattrInline` (48 bytes in the inode) holds entries back to back from offset 0;
`xattrInlineUsed` is their total size. An inline value is at most `48 - 4 - nameLen` bytes.

### 9.3 Overflow block

At most one per inode (`xattrBlock`), owned by that inode alone (never shared). It is journaled
metadata.

**Xattr block header:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | u32 | magic | `0x42544158` (bytes "XATB") |
| 4 | 2 | u16 | entryCount | 1..; a block with no entries is freed and `xattrBlock` set to 0 |
| 6 | 2 | u16 | usedBytes | bytes used in the entry area, `<= 4064` |
| 8 | 8 | u64 | selfBlock | this block's number |
| 16 | 8 | u64 | ownerIno | owner inode |
| 24 | 4 | u32 | ownerGeneration | owner's `generation` |
| 28 | 4 | u32 | csum | `metaCsum(block[0..4095])` with this field as zero |

Total size: 32 bytes

**Xattr block:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 32 | XattrBlockHeader | header | above |
| 32 | 4064 | u8[4064] | entries | packed sorted entries, then zeros |

Total size: 4096 bytes

A block value is at most `4064 - 4 - nameLen` bytes.

### 9.4 Placement policy

- **Set:** remove any existing entry with that name (from whichever area holds it). Then place the
  new entry inline if it fits in the remaining inline space; otherwise in the block (allocating it
  if needed); otherwise fail with `STATUS_ERR_NO_SPACE`. No rebalancing between the areas.
- **Remove:** delete the entry and repack its area. Free the block when it becomes empty.

---

## 10. Directories

### 10.1 Names and collation

- A name is 1..255 bytes. It MUST NOT contain `/` (0x2F) or NUL (0x00), and MUST NOT be `.` or
  `..`, which are never stored (`.` is implicit; `..` is the directory's `parentIno` field, §10.4 and
  §10.5).
- Names are compared **byte-wise** (unsigned, like `memcmp`, with a proper prefix sorting first).
  There is no Unicode normalization and no case folding: two names are equal only if their bytes
  are equal. Names are expected to be UTF-8, but the format stores any byte string that follows
  the rules above (see Q5 for whether the VFS rejects invalid UTF-8).
- Directory **order** (storage order and `readdir` order) is by `(hash(name), name)`: hash first,
  then name bytes to break ties.

### 10.2 Hash function

`hash(name) = SipHash-2-4(k, name)`, a 64-bit value, where `k` is the superblock's 16-byte
`dirHashKey` (`k0 = le64(dirHashKey[0..7])`, `k1 = le64(dirHashKey[8..15])`) and the message is
the raw name bytes. SipHash-2-4 is defined by Aumasson and Bernstein, "SipHash: a fast short-input
PRF" (2012), reference implementation `siphash24.c`; it is integer-only (64-bit add, rotate,
XOR).

Why a keyed PRF and not a simple seeded hash: two different names with the same 64-bit hash have
to share a leaf (§10.6), so an attacker who can choose names in a shared directory (`/tmp`) must
not be able to produce collisions. With a random per-filesystem key that nobody without raw-disk
access can read, finding even one collision is infeasible.

Known answers (host tests MUST check them):

| Key | Message | SipHash-2-4 |
|---|---|---|
| `00 01 02 ... 0F` | empty | `0x726FDB47DD0E0E31` |
| `00 01 02 ... 0F` | the 15 bytes `00 01 ... 0E` | `0xA129CA6149BE45E5` |
| `00 01 02 ... 0F` | ASCII `hello` | `0x004FB3985767DF81` |
| `00 01 02 ... 0F` | ASCII `lost+found` | `0x07DCADAE645E6DC1` |
| 16 zero bytes | ASCII `lost+found` | `0x984AE064A79DA99E` |

(The first two are vectors 0 and 15 of the reference implementation's test table.)

### 10.3 Two formats

A directory starts **inline** (`INLINE` set, entries in `iData`). When an insert doesn't fit, it
converts, in the same transaction as that insert, to a **hashed** directory: a B+tree of 4 KiB
nodes stored in the directory inode's own logical blocks (mapped by its extent tree, like file
data, but journaled as metadata). A hashed directory never converts back in v1.

### 10.4 Inline directory (in `iData`)

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | parentIno | the `..` target: the directory containing this one's entry; the root's is 2 |
| 8 | 2 | u16 | usedBytes | bytes used in `entries`, 0..96 |
| 10 | 2 | u16 | entryCount | number of entries |
| 12 | 4 | u32 | reserved | 0 |
| 16 | 96 | u8[96] | entries | packed entries in `(hash, name)` order, then zeros |

Total size: 112 bytes

**Inline entry** (fixed part; followed by `nameLen` name bytes, no padding):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | ino | target inode, `>= 16` and `<= inodeCount` |
| 8 | 1 | u8 | nameLen | 1..255 (in practice at most 86 here) |
| 9 | 1 | u8 | fileType | 1..7, MUST match the target's mode (§7.3) |

Total size: 10 bytes

Inline entries don't store the hash; it is recomputed from the name when needed.

### 10.5 Hashed directory: node format

Every logical block `0 .. size/4096 - 1` of a hashed directory is mapped (written extents, no
holes) and is exactly one of: the **root** (always logical block 0), an in-use **index** or
**leaf** node reachable from the root, or a **free** node on the directory's free-node list.

**Directory node header:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | u32 | magic | `0x4E524944` (bytes "DIRN") |
| 4 | 2 | u16 | level | 0 = leaf, 1..3 = index node height above the leaves; `0xFFFF` = free node. The root's level is the tree height minus 1 |
| 6 | 2 | u16 | count | entries in the node. Leaf: 0.. (0 only in the root); index: 2..253 in the root, 1..253 elsewhere; free: 0 |
| 8 | 8 | u64 | ownerIno | the directory's inode number |
| 16 | 4 | u32 | ownerGeneration | the directory's `generation` |
| 20 | 4 | u32 | logicalBlock | this node's logical block number in the directory |
| 24 | 8 | u64 | parentIno | root (logical 0) only: the `..` target (the root directory's is 2). 0 in every other node |
| 32 | 2 | u16 | usedBytes | bytes used in the entry area: leaf = sum of entry sizes; index = `count * 16`; free = 0 |
| 34 | 2 | u16 | reserved | 0 |
| 36 | 4 | u32 | nextFree | free nodes only: logical block of the next free node, 0 = end of list. 0 otherwise |
| 40 | 4 | u32 | freeHead | root only: logical block of the first free node, 0 = none. 0 otherwise |
| 44 | 4 | u32 | csum | `metaCsum(block[0..4095])` with this field as zero |

Total size: 48 bytes

**Directory node:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 48 | DirNodeHeader | header | above |
| 48 | 4048 | u8[4048] | entryArea | packed entries from offset 0, then zeros (a free node's area is all zero) |

Total size: 4096 bytes

**Leaf entry** (fixed part; followed by `nameLen` name bytes, no padding; entry size is
`18 + nameLen`, at most 273, so a leaf always holds at least 14 entries):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | hash | MUST equal `SipHash-2-4(dirHashKey, name)` |
| 8 | 8 | u64 | ino | target inode, `>= 16` and `<= inodeCount` |
| 16 | 1 | u8 | nameLen | 1..255 |
| 17 | 1 | u8 | fileType | 1..7, MUST match the target's mode |

Total size: 18 bytes

**Index entry:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | key | separator hash; stored as 0 in entry 0 of every index node |
| 8 | 4 | u32 | childLogical | logical block of the child node (never 0, since the root is never a child) |
| 12 | 4 | u32 | reserved | 0 |

Total size: 16 bytes

An index node holds at most 253 entries (`4048 / 16`).

### 10.6 Hashed directory: invariants

1. **Ranges.** The root covers hashes `[0, 2^64)`. Child `i` of an index node covering `[lo, hi)`
   covers `[key[i], key[i+1])`, reading `key[0]` as `lo` and `key[count]` as `hi`; `key[0]` is
   stored as 0; `key[1..count-1]` strictly increase inside `(lo, hi)`.
2. Leaf entries are strictly increasing by `(hash, name)` and every hash lies in the leaf's
   range. So all entries with one hash value are in one leaf.
3. Children have `level = parent level - 1`; the root's level is at most 3 (at most 4 node levels).
4. Every in-use node except the root has `count >= 1`.
5. The free list starting at the root's `freeHead` visits each free node once and ends with 0.
   In-use nodes (each reachable exactly once) and free nodes together are exactly the logical
   blocks `0 .. size/4096 - 1`.

### 10.7 Hashed directory: operations

Let `h = hash(name)` and `sz = 18 + nameLen`.

**Lookup:** from the root, at an index node take the last entry `i` with `i == 0` or `key[i] <=
h`; at the leaf, scan for the entry with hash `h` and equal name bytes. Verify every node read:
magic, checksum, `ownerIno`, `ownerGeneration`, `logicalBlock`, level, `count`, `usedBytes`
(entries parse exactly to `usedBytes`). The path is at most 4 nodes.

**Allocating a node:** if the root's `freeHead != 0`, pop that node (`freeHead = node.nextFree`).
Otherwise append: logical block `size / 4096` is mapped with a newly allocated block (§8.6) and
`size += 4096`; if that logical number would reach 2^32, fail with `STATUS_ERR_NO_SPACE`.

**Freeing a node** (never the root): rewrite it as a free node (`level = 0xFFFF`, `count = 0`,
`usedBytes = 0`, zero entry area, `nextFree = root.freeHead`), and set `root.freeHead` to it.
Directory blocks are never returned to the block allocator while the directory exists (a
directory never shrinks, as in ext4); `rmdir` frees them all.

**Insert:**
1. Look up; if an entry with equal name exists: `STATUS_ERR_EXISTS`.
2. If the leaf has `usedBytes + sz <= 4048`: insert in `(hash, name)` order. Done.
3. Otherwise split the leaf. Form the `n` sorted entries (the new one included) with sizes
   `s[0..n-1]` and total `T`. Take `k` = the smallest index in `[1, n-1]` with
   `s[0] + ... + s[k-1] >= T / 2`. If `hash[k-1] == hash[k]`, move `k` up until they differ; if
   that reaches `n`, instead move `k` down from the original value until they differ; if that
   reaches 0, fail with `STATUS_ERR_NO_SPACE`. If either half is larger than 4048 bytes, fail with
   `STATUS_ERR_NO_SPACE`. (Both failures need many names with an identical 64-bit keyed hash.)
   The left half `[0, k)` stays in the node; a newly allocated node gets `[k, n)`; the separator
   is `hash[k]`.
4. Insert `{separator, newNode}` into the parent right after the split node's entry. If the parent
   already has 253 entries, split it: of the 254 entries the left keeps `[0, 127)`, a new node
   gets `[127, 254)`, the separator is the new node's first key (then stored as 0 there), and the
   separator goes into the grandparent. Repeat upward.
5. **Root split** (the root is the node that must split, leaf or index): allocate two nodes `A`
   and `B`; `A` gets the left half and `B` the right half (both at the root's old level); the
   root becomes an index node at `level + 1` with entries `{0, A}` and `{separator, B}`, keeping
   its `parentIno` and `freeHead`. If the new level would be 4: `STATUS_ERR_NO_SPACE`.
6. Update the directory's `mtime`/`ctime`, and the target's `linkCount` (§12.1).

Before the first modification, the insert determines how many nodes it needs (at most one per
split level plus one for a root split: at most 5) and reserves them (free list first, then
appended blocks), so a failure leaves nothing half-done.

**Remove:**
1. Look up; if absent: `STATUS_ERR_NOT_FOUND`. Remove the entry from its leaf.
2. If the leaf is the root: done.
3. If the leaf is now empty: free it and remove its entry from the parent (if that was entry 0,
   store the new entry 0's key as 0). If the parent (not the root) is now empty, repeat for it.
4. Otherwise, if the leaf's `usedBytes < 1012` (a quarter of 4048): take its right sibling under
   the same parent if there is one, else its left sibling. If the two nodes' `usedBytes` sum to
   `<= 3036` (three quarters), move all entries of the right-hand node of the pair into the
   left-hand one, free the right-hand one, and remove its parent entry (never entry 0).
5. **Collapse:** while the root is an index node with exactly one entry: copy that child's
   `level`, `count`, `usedBytes`, and entry area into the root (keeping the root's `ownerIno`,
   `ownerGeneration`, `logicalBlock` 0, `parentIno`, and `freeHead`), then free the child.
6. Update the directory's `mtime`/`ctime`.

Index nodes are never merged; they are freed only when empty.

**Inline to hashed conversion** (an insert into an inline directory whose `usedBytes + 10 +
nameLen > 96`): allocate one block, write it as the root leaf (logical 0) holding the inline
entries plus the new one with their hashes, `parentIno` copied from the inline header,
`freeHead = 0`; clear `INLINE`; `iData` becomes an extent root mapping logical 0 to the block;
`size = 4096`; `allocatedBlocks += 1`. All in the insert's transaction.

**Capacity.** An insert fails only when the root is at level 3 and full (253 entries) and a split
must propagate into it. A directory grown by inserts alone therefore holds at least about
`253 * 127 * 127 * 6`, roughly 24 million entries of 255-byte names (index nodes at least half
full after a split, leaves at least 6 maximum-size entries), and far more with short names.
Because index nodes aren't merged, heavy deletion followed by insertion into one narrow hash range
can reach the limit sooner; the failure is `STATUS_ERR_NO_SPACE`, never corruption.

### 10.8 Inline directory operations

Entries stay sorted by `(hash, name)` (the hash computed on the fly). Insert fails over to the
conversion above; remove repacks. The inline format has no parent-entry or free-list mechanics.

### 10.9 readdir

`readdir` returns `.`, `..`, then the entries in `(hash, name)` order: for an inline directory
in stored order, for a hashed one by walking the tree left to right with an explicit path stack
(at most 4 nodes; no recursion, no sibling pointers). Order is identical across the inline to
hashed conversion.

Resuming (`telldir`/`seekdir`, a 64-bit position) is a driver matter, not on-disk format. The
recommended mapping is position 0 = start, 1 = after `.`, 2 = after `..`, and for an entry with
hash `h` the position `(h >> 2) + 3`, resuming at the first entry whose `(h >> 2) + 3` is greater
than the given position. Entries that share `h >> 2` with the last returned entry may then repeat
or be skipped after a seek; a sequential readdir that keeps its full `(hash, name)` cursor in the
open-file object has neither problem.

---

## 11. Symlinks, device nodes, FIFOs, sockets

### 11.1 Symlinks

The target is 1..4095 bytes, containing no NUL (so it fits `PATH_MAX` = 4096 with a
terminator). It is stored without a terminator; `size` is its length.
- **Short** (1..112 bytes): `INLINE` set, the target in `iData[0 .. size-1]`, the rest zero.
  Every target of 112 bytes or less MUST be stored inline.
- **Long** (113..4095 bytes): one data block holding the target at offset 0, mapped as logical
  block 0 by a single written extent; bytes past `size` in that block are unspecified. The block
  is file data (ordered, §16.5), not journaled metadata, and is written once, at creation. A
  symlink's target never changes after creation.

### 11.2 Device nodes

`iData` = `u32 major` at offset 0, `u32 minor` at offset 4, then 104 zero bytes. `size = 0`. The
numbers are stored for ports and the Linux layer; the native devfs is synthetic and doesn't use
them.

### 11.3 FIFOs and sockets

`iData` is zero and `size = 0`. A socket inode is what a path-bound local socket leaves behind;
the filesystem only stores it.

---

## 12. Hard links and the orphan list

### 12.1 Link counts

- A non-directory's `linkCount` is the number of directory entries naming it (hard links).
- A directory is named by exactly one entry (the root by none), and its `linkCount` is
  `2 + number of subdirectories` (its entry or, for the root, the root itself; its own `.`; each
  child's `..`). Directory hard links are forbidden.
- `link()` on a directory fails (`STATUS_ERR_INVALID` at the VFS; the format forbids it). A link
  that would push `linkCount` past `0x7FFFFFFF` fails with `STATUS_ERR_TOO_MANY_LINKS`.
- Renaming a directory to a different parent updates, in one transaction: both parents' entries,
  the moved directory's `parentIno` (inline header or root node), and both parents' `linkCount`.

### 12.2 The orphan list

An **orphan** is an inode whose on-disk state isn't final yet and must be finished after a
crash. Two cases:
1. **Unlinked but still open:** `linkCount` reached 0 while a handle (or mapping) still refers to
   the inode. Its blocks are released when the last reference goes away.
2. **Truncate in progress:** a size reduction that frees more than one transaction can hold
   (§16.3). `TRUNCATING` is set.

The list is singly linked: superblock `orphanHead`, then each inode's `orphanNext`, ending at 0.
Members have the `ORPHAN` flag; non-members have it clear and `orphanNext = 0`. The superblock
block and the inode are journaled metadata, so list changes are atomic with the operations that
cause them. New orphans are pushed at the head; removal walks from the head to find the
predecessor (the list is short).

**Deleting an inode** (`linkCount` becomes 0):
1. In the transaction that removes the last directory entry: `linkCount = 0`, push onto the
   orphan list, `ctime = now`.
2. When no references remain (immediately if none): free the inode's blocks from the end of the
   file backward, at most `BFS_TRUNCATE_CHUNK` (8) extent entries per transaction, the inode
   staying on the list. Free the xattr block, then all extent-tree blocks.
3. Final transaction: remove from the orphan list, clear the inode's bitmap bit (and decrement
   `usedDirs` for a directory), write the free form (§7.7).

An implementation MAY do steps 1-3 in one transaction when the whole deletion fits in that
transaction's credits (§16.3); every intermediate committed state is valid either way.

**Truncating down** from `S` to `S' < S`, when freeing `[ceil(S'/4096), ...)` needs more than one
transaction:
1. First transaction: `size = S'`, set `ORPHAN` and `TRUNCATING`, push onto the list. The written
   extents now past EOF are allowed only because `TRUNCATING` is set (§8.4 invariant 6).
2. Then free extents from the end, chunked as above.
3. Final transaction: clear `TRUNCATING` and `ORPHAN`, remove from the list.

A truncate that fits in one transaction just sets `size` and frees the extents together.

**Recovery** (mount, read-write, after replay; also `fsck.bongfs` in repair modes): walk the list
from the head, at most `inodeCount` steps (a longer walk means a cycle: `STATUS_ERR_CORRUPT`).
For each inode: if `linkCount == 0`, finish the deletion (steps 2-3); else if `TRUNCATING`, free
every extent at or past `ceil(size / 4096)` (written or unwritten) and finish the truncate;
otherwise just remove it from the list (log a warning). A read-only mount leaves orphans alone.

---

## 13. Allocation policy

Allocation is **deterministic**: given the on-disk state and the sequence of operations, the
chosen inode and block numbers are always the same (no randomness, no clock). Implementations may
cache per-group summaries in memory, but the result MUST equal the reference algorithm below. The
M7.6 fuzzer relies on this to reproduce failures from a seed.

Averages below are integer means over all groups: `avgFreeInodes = totalFreeInodes / groupCount`,
`avgFreeBlocks = totalFreeBlocks / groupCount`.

### 13.1 Choosing an inode's group

- **Directory whose parent is the root** (top-level spreading): start at group
  `s = (hash(name) >> 32) % groupCount` and take the first group, going up from `s` and wrapping,
  with `freeInodes >= max(1, avgFreeInodes)` and `freeBlocks >= avgFreeBlocks`. If none
  qualifies, take the group with the most free inodes (lowest index on ties).
- **Other directory:** start at the parent's group `p`; take the first group from `p` (wrapping)
  with `freeInodes >= 1` and `freeBlocks >= avgFreeBlocks / 4`. If none qualifies, the first
  group from `p` with a free inode.
- **Non-directory:** the parent directory's group if it has a free inode; otherwise the first
  group after it (wrapping) with a free inode.
- No free inode anywhere: `STATUS_ERR_NO_SPACE`.

### 13.2 Choosing the slot

The lowest free index in the chosen group's inode bitmap (§7.7).

### 13.3 Block goal

- **File data at logical block `L`:** if some extent starts at or before `L`, take the one with
  the greatest start, `e`, and use `goal = e.physicalBlock + (L - e.logicalBlock)`. Otherwise
  `goal = firstData(group of the inode)`.
- **Directory nodes, extent-tree blocks, xattr blocks, long-symlink blocks:**
  `goal = firstData(group of the inode)`.

### 13.4 Block search

`allocRun(goal, want, min)` returns one run of `n` contiguous free blocks, `min <= n <= want`:

1. `w = want`.
2. Visit groups starting with `g0 = group(goal)`, then `g0 + 1`, ..., wrapping, each once. Skip a
   group whose `freeBlocks < w`. In `g0`, scan the bitmap from `goal` to the group end, then from
   `firstData(g0)` to `goal`; in other groups scan from `firstData(g)`. Take the **first** run of
   at least `w` free, non-pinned (§13.5) blocks, and return its first `w` blocks.
3. If none was found and `w > min`: `w = max(min, w / 2)`, go to step 2.
4. Otherwise `STATUS_ERR_NO_SPACE` (after the reserve check, §14.2, and after forcing a commit and
   checkpoint once to release pinned blocks and retrying).

A multi-block request (a large write) calls `allocRun` repeatedly with the next goal right after
the previous run. Runs never cross a group boundary (every group starts with metadata).

### 13.5 Deferred frees (pinning)

Freed blocks are not immediately reusable. When a transaction `T` frees a block `b`:
- `b` stays marked used in the **in-memory** allocator view until `T` commits (the on-disk bitmap
  change is part of `T`). Reusing it earlier could hand out a block that the last committed state
  still references.
- If `b` has a logged copy in any transaction still in the live journal (between the tail and
  the head, §16) or in `T` itself, `b` stays unusable for **file data** (anything not journaled: data blocks,
  long-symlink blocks, unwritten extents) until the checkpoint moves the journal tail past `T`
  (§16.6). It may be reused earlier as journaled metadata. This rule is what lets the journal
  work without revoke records (§15.6).

The pinned sets are in memory only, bounded by the number of blocks freed per transaction plus
the number of distinct blocks in the live journal (at most `journalBlocks`). After a mount they
are empty.

### 13.6 Fragmentation, delayed allocation, discard

- **Delayed allocation** (kernel driver, M7.7): buffered writes MAY defer block allocation to
  writeback so that consecutive writes get one run. The driver MUST reserve, at `write()` time,
  the worst-case block count (data + extent-tree growth) in an in-memory counter, so writeback
  never fails with `STATUS_ERR_NO_SPACE`. Reservations are never on disk; a crash loses only the
  unwritten data, never consistency. Host tools allocate at write time.
- **Anti-fragmentation:** allocate the whole request as one run when possible (§13.4), keep file
  data after the file's previous extent (§13.3), spread top-level directories (§13.1), and keep
  the 5% reserve (§14.2), which keeps the allocator away from the nearly-full regime where
  fragmentation grows fastest.
- **Discard/TRIM:** a mount option (`discard`, default **off**). A freed block may be discarded
  only once it is no longer pinned (§13.5). It needs a discard operation in `BlockDevice`, which
  ARCHITECTURE §15 doesn't have yet; until then the option is refused. Under full-disk encryption
  discard leaks which blocks are free (see Q8).

---

## 14. Free-space accounting and reserved blocks

### 14.1 Sources of truth

1. The **bitmaps** are authoritative.
2. The **group descriptor counters** (`freeBlocks`, `freeInodes`, `usedDirs`) are journaled with
   the bitmaps, so after any commit or replay they equal the bitmap counts exactly.
3. The **superblock hints** (`freeBlocksHint`, `freeInodesHint`) are written only at clean
   unmount (the sums of the descriptor counters) and are never trusted: mount always sums the
   descriptors. When a transaction logs the superblock block (for the orphan list), the hints in
   that copy are whatever was in memory and mean nothing.

`fsck.bongfs` rebuilds everything from the metadata it walked (§22 pass 5): bitmaps from the
blocks and inodes actually referenced, counters from the bitmaps, hints from the counters.

### 14.2 Reserved blocks

`statfs` reports `free = sum(freeBlocks) - pinned - delalloc reservations`, and
`available = free - reservedBlocks` (not below 0). An allocation by a caller whose effective uid
isn't `reservedUid`, and who isn't in group `reservedGid`, fails with `STATUS_ERR_NO_SPACE` when
it would leave `free < reservedBlocks`. Metadata allocations made while removing things (for
example an extent split during a punch) are never subject to the reserve, so deleting always
works on a full filesystem, except for a punch or rename that needs a new block when none are
free at all.

---

## 15. Journal: on-disk format

### 15.1 Placement and positions

The journal is `journalBlocks` contiguous blocks in group 0 starting at `journalStart` (§3.2),
marked used in the block bitmap. It has no inode. **Position** `p` (0 <= p < `journalBlocks`) is
block `journalStart + p`.
- Positions 0 and 1 are the two **journal superblock slots** (§15.2).
- Positions `2 .. journalBlocks - 1` are the circular **log**, `N = journalBlocks - 2` blocks.
  `next(p) = (p + 1 == journalBlocks) ? 2 : p + 1`.

`maxTxnBlocks = journalBlocks / 4` (at least 256) is the largest log footprint of one
transaction.

A transaction occupies consecutive log positions (wrapping):
`D1, copies..., D2, copies..., ..., Dk, copies..., C`, where each `Di` is a descriptor block
followed by exactly `tagCount` logged block copies, and `C` is the commit block. `k >= 1`, and
every descriptor has 1..254 tags.

### 15.2 Journal superblock (two slots)

The journal superblock is written directly (never logged). Two slots make its update atomic: an
update writes the slot that does **not** hold the current copy, with `updateCount + 1`. Mount
uses the valid slot with the larger `updateCount` (slot 0 on a tie; a tie means a bug). As with
the superblock, all fields are in the first 512 bytes.

**Journal superblock block:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 512 | JournalSuperblock | jsb | below |
| 512 | 3584 | u8[3584] | reserved | zero; not checksummed |

Total size: 4096 bytes

**Journal block common header** (first 24 bytes of the journal superblock, descriptor, and
commit blocks; logged copies have no header):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | u32 | magic | `0x4C4E524A` (bytes "JRNL") |
| 4 | 4 | u32 | blockType | 1 = journal superblock, 2 = descriptor, 3 = commit. 4 is reserved (a future revoke block, which would be an incompat feature); any other value is invalid |
| 8 | 8 | u64 | seq | transaction sequence number; 0 in a journal superblock |
| 16 | 4 | u32 | logPos | the position this block was written at (the slot number, 0 or 1, for a journal superblock) |
| 20 | 4 | u32 | csum | per §2.3 |

Total size: 24 bytes

**Journal superblock:**

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 24 | JournalHeader | header | `blockType` 1, `seq` 0, `logPos` = slot |
| 24 | 4 | u32 | journalBlocks | MUST equal the superblock's |
| 28 | 4 | u32 | maxTxnBlocks | MUST equal `journalBlocks / 4` |
| 32 | 8 | u64 | updateCount | >= 1; +1 on each update |
| 40 | 8 | u64 | tailSeq | the sequence number replay expects first; >= 1 |
| 48 | 4 | u32 | tailPos | the log position where that transaction starts; 2..`journalBlocks - 1` |
| 52 | 4 | u32 | fsErrorFlags | bit 0: the filesystem hit an error after this journal's transactions were committed (§18.3); replay copies it into the superblock's `ERRORS`. Other bits 0 |
| 56 | 16 | u8[16] | fsUuid | MUST equal the superblock's `uuid` |
| 72 | 440 | u8[440] | reserved | 0 |

Total size: 512 bytes

The journal superblock records only the **tail**. The head is found by scanning (§17).

### 15.3 Descriptor block

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 24 | JournalHeader | header | `blockType` 2, `seq` of the transaction, `logPos` |
| 24 | 2 | u16 | tagCount | 1..254 |
| 26 | 2 | u16 | reserved | 0 |
| 28 | 4 | u32 | reserved | 0 |
| 32 | 4064 | u8[4064] | tags | 254 tag slots of 16 bytes; slots past `tagCount` are 0 |

Total size: 4096 bytes

**Tag** (tag `i` describes the `i`-th block after this descriptor):

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | targetBlock | home block number of the copy: `1 <= targetBlock < blockCount`, outside the journal |
| 8 | 4 | u32 | dataCsum | `metaCsum(le64(seq), le64(targetBlock), copy[0..4095])` |
| 12 | 4 | u32 | reserved | 0 |

Total size: 16 bytes

A transaction logs each target block **at most once** (the copy is the block's final state in
that transaction).

### 15.4 Commit block

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 24 | JournalHeader | header | `blockType` 3, `seq`, `logPos` |
| 24 | 4 | u32 | txnLogBlocks | log blocks in the transaction, descriptors and this commit block included; `<= maxTxnBlocks` |
| 28 | 4 | u32 | firstLogPos | position of the transaction's first descriptor |
| 32 | 4 | u32 | descriptorCount | `k` |
| 36 | 4 | u32 | dataBlockCount | sum of all `tagCount`s; `txnLogBlocks = descriptorCount + dataBlockCount + 1` |
| 40 | 8 | s64 | commitTimeNs | when the commit was written (informational) |
| 48 | 4048 | u8[4048] | reserved | 0 |

Total size: 4096 bytes

The commit block's checksum covers the whole block, so a torn commit write never validates.

### 15.5 Sequence numbers

`seq` is a `u64` that increases by exactly 1 per transaction and never wraps in practice
(2^64). `mkfs.bongfs` sets the first `tailSeq` to a random value in `[1, 2^32]` (a fixed value in
deterministic mode, §19) and zeroes the whole journal area. A block in the log is only accepted
as part of transaction `s` if its header says `seq == s` **and** `logPos` is the position it was
read from **and** its checksum (seeded by the UUID) matches, so stale blocks from an earlier lap
of the log (lower `seq`), from a torn transaction (§17 step R5 skips its `seq`), or from an
earlier filesystem (different UUID) are never replayed.

### 15.6 No revoke records, no escaping

**Revoke records** exist in jbd2 to stop replay from writing an old metadata copy over a block
that was freed and then reused for unjournaled file data. bongfs prevents that case at the
allocator instead: a freed block that has a copy in the live log isn't reused for file data until
the checkpoint has moved the tail past the freeing transaction (§13.5). Reuse as metadata is safe
without that wait, because every metadata write is journaled, so the newer copy is later in the
log and wins. Result: replay needs no revoke table, which keeps its memory O(1) (a revoke table
is unbounded: up to about 254 entries per log block), and the format stays smaller. Cost: freed
metadata blocks become reusable for data only after a checkpoint, and an allocator that hits
`STATUS_ERR_NO_SPACE` forces a commit + checkpoint and retries (§13.4).

**Escaping** (jbd2 marks logged copies that happen to start with its magic) isn't needed: replay
never interprets a logged copy as a header, because the descriptor says exactly how many copies
follow, and a copy is only ever read at a position the descriptor assigns to it.

---

## 16. Journal: transactions, commit, checkpoint

### 16.1 Terms

- **Metadata** (always journaled): the primary superblock block (block 1), GDT blocks, bitmaps,
  inode-table blocks, extent-tree blocks, directory nodes of hashed directories, xattr blocks.
- **Data** (never journaled): file data blocks and long-symlink blocks. Their writes are
  *ordered* against commits (§16.5).
- **Direct writes** (neither): the superblock at mount/unmount/error, the journal superblock, and
  backup superblocks, each at points fixed in §16.8 and §18.
- **Handle:** one filesystem operation's membership in a transaction, with a credit reservation
  (§16.3). Operations are atomic: all of a handle's changes are in one transaction.
- **Running** transaction: accepts new handles. **Committing:** closed, being written. At most one
  of each exists at a time.
- **Checkpointed:** every block the transaction logged has been written to its home location and
  the journal tail has moved past it.

### 16.2 Atomicity rule

An operation checks everything that can fail (name validity, existence, permissions, space,
depth and level limits) and reserves every block and inode it may need **before** it modifies
the first metadata block. After the first modification it cannot fail, except by an I/O error or
detected corruption, which aborts the journal (§18.3): the running transaction is then discarded
and never committed. So a committed transaction always contains whole operations.

### 16.3 Credits and transaction size

A handle declares its worst-case count of distinct metadata blocks ("credits"). A transaction's
log footprint is `m + ceil(m / 254) + 1` for `m` logged blocks, and MUST be `<= maxTxnBlocks`.
A new handle joins the running transaction only if the sum of reserved credits still fits;
otherwise it waits until that transaction is committed and joins the next one.

Worst cases for v1 (the implementation MAY reserve these constants per operation type):

| Component or operation | Credits | Derivation |
|---|---|---|
| allocate or free one run of blocks, or one inode | 2 | bitmap block + GDT block |
| extent insert (`E`) | 22 | up to 4 tree blocks on the path + up to 5 new ones (4 splits, 1 push-down) + 6 runs allocated (5 tree blocks + the data run) x 2 + the inode block |
| directory insert (`Dins`) | 119 | 4 path nodes + 5 new nodes + 5 appended logical blocks, each an `E` (110). Uses the free-node list first, so usually far fewer |
| directory remove (`Drem`) | 6 | 4 path nodes + 1 sibling + the directory inode (freed nodes go to the free list, not the allocator) |
| create, mknod, mkdir, short symlink | 122 | inode allocation 3 (bitmap, GDT, table block) + `Dins` |
| long symlink | 124 | create + one data block allocation (its extent fits in the inline root) |
| link | 120 | `Dins` + target inode |
| unlink, rmdir (inode goes on the orphan list) | 8 | `Drem` + target inode + superblock (orphan push) |
| rename | 127 | `Drem` + `Dins` + moved inode + moved directory's parent field (1). Replacing an existing target rewrites that entry in place (no `Dins`) and orphans the victim: at most 12 |
| setattr, truncate that fits one transaction without splitting | 1 + frees | the inode block + 2 per freed run and tree block |
| set or remove an xattr | 4 | inode + xattr block + its allocation or free (2) |
| map one written run (write into a hole or past EOF) | 22 | `E` |
| convert one unwritten run to written | 44 | two `E` |
| one truncate or delete chunk (`BFS_TRUNCATE_CHUNK` = 8 extents removed from the end) | 116 | 8 x (4 path blocks + data-run free 2 + up to 4 freed tree blocks x 2) + inode + orphan-list update (superblock or predecessor, 3 at most) |
| final orphan removal (free the inode) | 5 | inode free 3 + superblock/predecessor inode 2 |

`BFS_MAX_OP_CREDITS = 192` bounds every single handle. Because `journalBlocks >= 1024`,
`maxTxnBlocks >= 256`, which holds `m = 254`, so any one handle always fits in an empty
transaction. Operations whose size is unbounded (writing or fallocating a large range, truncate,
deleting a large file) are split into a sequence of handles, each a valid state on its own: a
write or fallocate maps some runs and sets `size` to cover only what is mapped; truncate and
delete use the orphan list (§12.2).

### 16.4 Commit algorithm

Commits are strictly serialized: transaction `T + 1` starts step C3 only after `T` finished C11.
The running transaction `T + 1` keeps accepting handles meanwhile.

1. **C1 Close.** `T` stops accepting handles; new handles go to `T + 1`. Wait until every handle
   of `T` has finished.
2. **C2 Order.** Wait until `T - 1` has completed C11.
3. **C3 Ordered data.** Submit the data writes on `T`'s ordered list (§16.5).
4. **C4 Freeze and checksum.** For each metadata block of `T`: take its frozen copy (its contents
   at C1: if `T + 1` modifies the block after C1, it works on a separate copy, copy-on-write), and
   compute into the frozen copy every checksum it contains, in this order: bitmap checksums into
   their GD slots, then GD checksums, inode checksums (every slot that changed), extent, directory
   and xattr block checksums, then the superblock checksum.
5. **C5 Space.** If the log has fewer than `txnLogBlocks` free positions, checkpoint (§16.6)
   until it does.
6. **C6 Log.** Write the descriptors and copies at the head positions, computing each tag's
   `dataCsum`. Writes may be issued in any order and concurrently with C3.
7. **C7 Wait** for every C3 and C6 write. On any I/O error: abort (§18.3); `T` is not committed.
8. **C8 FLUSH** the device.
9. **C9 Commit block.** Write the commit block at the next position and wait for it.
10. **C10 FLUSH** the device. (A device with FUA could replace C9+C10 with one FUA write, but
    `BlockDevice` only exposes `flush`, so v1 always flushes.)
11. **C11 Done.** The head moves past the commit block. `T`'s blocks join the checkpoint list.
    Blocks freed in `T` become reusable per §13.5. Wake every `fsync` waiting on `T`.

The FLUSH at C8 is what makes a valid commit block imply that every other block of the
transaction (and its ordered data) is durable. Without it, a device that reorders writes could
persist the commit block alone.

### 16.5 Ordered mode

**Guarantee.** After a crash and recovery, every block that the recovered metadata maps as
written for a file holds data written to that file by the filesystem (by an application write,
or zeros written by the filesystem itself, for example the tail zeroing of §8.4). It never holds
another file's old contents or unallocated-disk garbage.

**Enforcement.** Each transaction `T` has an ordered list of `(inode, logical range)` items. An
item is added when `T`:
1. maps new blocks as written (a write into a hole or past EOF; delayed allocation at writeback),
2. converts unwritten blocks to written,
3. raises `size`: the block containing the old EOF (so its tail zeroing, §8.4 invariant 7, and any
   bytes appended into it are on disk first), and
4. creates a long symlink (its target block).

At C3 the dirty page-cache data in those ranges is written, and C7 waits for it, so it is durable
at C8, before the commit block exists.

**Not guaranteed.** Overwrites of blocks that were already mapped and written aren't ordered
against commits. After a crash such a block holds the old data, the new data, or (at sector
granularity) a mix. Applications use `fsync` for durability.

**Replace-by-rename.** When `rename` replaces an existing regular file, the driver MUST add all
of the source file's dirty data to the rename's transaction's ordered list. This makes the common
"write a temporary file, then rename it over the original" pattern crash-safe without `fsync`
(after a crash the name refers to the old file or to the complete new one, never to an empty or
partial new one).

### 16.6 Checkpoint

1. **K1** Pick the committed transactions to retire: a prefix `tailSeq .. X` of the log.
2. **K2** For each block logged in them: if a committed transaction later than `X` logged it too,
   skip it (that one's checkpoint will write it). Otherwise write the block's **newest committed**
   contents (the frozen copy from the newest committed transaction that logged it, never
   uncommitted changes of the running transaction) to its home location.
3. **K3** Wait for the writes, then FLUSH.
4. **K4** Write the journal superblock to the other slot: `updateCount + 1`,
   `tailSeq = X + 1`, `tailPos` = the position after `X`'s commit block, `fsErrorFlags`
   unchanged. Wait, then FLUSH.
5. **K5** The retired transactions' log positions are now free. Release the file-data pins of
   blocks freed in transactions `<= X` (§13.5).

Positions of retired transactions MUST NOT be overwritten before K4's FLUSH completes: until
then, a crash replays from the old tail and needs those blocks intact.

Triggers: free log space below `2 * maxTxnBlocks` (checked before C5), unmount, the allocator's
`STATUS_ERR_NO_SPACE` retry, and (policy) 30 s of journal idleness.

### 16.7 Commit triggers, group commit, fsync

- A commit of the running transaction starts when: an `fsync`/`fdatasync`/`sync` needs it, the
  commit timer fires (default 5 s after the transaction's first handle; mount option
  `commit=1..300` seconds), a handle doesn't fit (§16.3), or unmount.
- **Group commit:** all handles in a transaction commit together; every `fsync` waiting on it is
  released by the same C11.
- **fsync(file):** write back the file's dirty data and wait; then, if any of the inode's metadata
  changes (tracked in memory as the last transaction that touched it) isn't committed yet, commit
  that transaction and wait for C11; otherwise issue a FLUSH. Either way, `fsync` returns only
  after a device flush that follows the data writes. `fdatasync` may skip the commit when only
  timestamps changed, but still flushes.
- **sync/`fsSync`:** commit the running transaction, wait, FLUSH.

### 16.8 Where FLUSH is required

| # | Point | Why |
|---|---|---|
| F1 | C8, before writing a commit block | a durable commit block must imply durable log blocks and ordered data |
| F2 | C10, before a commit counts as done (before waking `fsync`) | the commit block itself must be durable |
| F3 | K3, before the journal superblock moves the tail | home locations must hold the data before the log copies can be discarded |
| F4 | K4, before retired log positions are reused | the new tail must be durable before the old log is overwritten |
| F5 | replay R5, after the replayed home writes and before the journal superblock update; and again after that update | replay must be complete before the log is declared empty, and the empty log must be durable before new transactions reuse it |
| F6 | mount, after setting `NEEDS_RECOVERY`, before the first transaction is written | tools that trust the flag must see it whenever the log might be non-empty |
| F7 | unmount, after the final checkpoint's journal superblock write (F3/F4), and again after the superblock write that clears `NEEDS_RECOVERY` | ordering of the clean-unmount marker |
| F8 | `fsync`, `sync` | durability contract |
| F9 | mkfs, before writing the primary superblock, and after it | the superblock (magic) appears last, so an interrupted mkfs leaves no mountable filesystem |
| F10 | fsck repair: after setting `ERRORS` at the start, after all repairs, and after the final superblock write | an interrupted repair leaves the filesystem flagged |
| F11 | error handler (§18.3), after the direct journal superblock and superblock writes | the error mark must survive |

The device is assumed to honor FLUSH (all writes completed before the flush are durable when it
completes). Writes issued after the last completed flush may be lost, reordered, or torn at
sector granularity; everything above is designed for exactly that model.

---

## 17. Journal: replay

Replay is a pure block copy: it reads only the journal and writes only home locations of logged
blocks (then the journal superblock and superblock). It never interprets filesystem metadata, never
allocates, and uses O(1) memory: a descriptor buffer and a copy buffer (a few KiB; with multiple
descriptors it re-reads them in pass B instead of keeping them).

Mount runs it every time, whatever `NEEDS_RECOVERY` says (the scan of a clean journal reads one
block). `fsck.bongfs` runs the same code (on disk in repair modes, into an in-memory overlay with
`-n`).

1. **R0 Preconditions.** The superblock passed §4.2; no unknown incompat bit. If replay turns out
   to be needed (step R3 finds a complete transaction) and the device is write-protected, stop:
   `STATUS_ERR_READ_ONLY`.
2. **R1 Journal superblock.** Read both slots. A slot is valid if: magic, `blockType == 1`,
   `seq == 0`, `logPos` == its slot number, checksum, `journalBlocks` and `fsUuid` equal the
   superblock's, `maxTxnBlocks == journalBlocks / 4`, `updateCount >= 1`, `tailSeq >= 1`,
   `2 <= tailPos < journalBlocks`, no unknown `fsErrorFlags` bit. Use the valid one with the
   larger `updateCount`. None valid: see the mount table (§5.3).
3. **R2 Start.** `pos = tailPos`, `seq = tailSeq`, `scanned = 0`.
4. **R3 Validate transaction `seq` at `pos` (pass A, reads only).**
   1. The block at `pos` must be a descriptor: magic, `blockType == 2`, `seq`, `logPos == pos`,
      checksum, `1 <= tagCount <= 254`. Otherwise: **end** (clean end of log).
   2. For each tag, in order: `targetBlock` in range (`1 <= targetBlock < blockCount`, not inside
      the journal); read the copy at the next position; check `dataCsum`. Remember whether any of
      these failed.
   3. The next block is either another descriptor for the same `seq` (repeat 2) or the commit
      block: magic, `blockType == 3`, `seq`, `logPos`, checksum, `firstLogPos` = the
      transaction's first position, `descriptorCount`/`dataBlockCount` = what was counted,
      `txnLogBlocks` = blocks consumed including the commit, `txnLogBlocks <= maxTxnBlocks`.
      Anything else: **end** (an incomplete transaction, the normal result of a crash).
   4. `scanned += txnLogBlocks`; if `scanned > N`: **end** (defensive loop bound).
   5. If the commit block is valid but step 2 recorded a failure, the transaction was committed
      and later damaged on the medium: set `corruptCommitted`, **end**. It and everything after it
      are not replayed; the result is the consistent state as of transaction `seq - 1`.
5. **R4 Apply (pass B).** For each tag of transaction `seq` in log order, re-read the copy and
   write it to `targetBlock`. (A copy appearing twice for one target in one transaction is a
   format violation; if it happens, the later one wins and a warning is logged.) Then
   `seq += 1`, `pos` = the position after the commit block; go to R3.
6. **R5 Finish.** Let `applied` = number of transactions applied, and `partial` = whether R3
   ended on a valid descriptor for `seq` (a torn transaction).
   - If `applied > 0`: FLUSH.
   - If `applied > 0` or `partial` or `corruptCommitted`: write the journal superblock to the other
     slot with `updateCount + 1`, `tailPos = pos`, `tailSeq = seq + 1` (skipping the torn
     transaction's number, so its leftover blocks can never match a future transaction),
     `fsErrorFlags |= corruptCommitted`; FLUSH. The journal is now empty.
   - If the resulting `fsErrorFlags` bit 0 is set, the superblock's `ERRORS` bit must be set (the
     mount does it in step R6's superblock write).
7. **R6 Superblock.** For a read-write mount, the superblock write of §18.1 step M5 sets
   `NEEDS_RECOVERY` (and `ERRORS` if required). For a read-only mount of a writable device, and
   for fsck: clear `NEEDS_RECOVERY`, set `ERRORS` if required, set `freeBlocksHint` and
   `freeInodesHint` to the sums of the descriptor counters as they are after replay, write the
   superblock, FLUSH. (Leaving stale hints next to a clear `NEEDS_RECOVERY` would make fsck report
   F5.4.)

**Idempotence.** Until R5, replay modifies only home locations, and only with copies taken from
a log it doesn't change. A crash anywhere in R4 leaves the journal superblock and log as they
were, so the next replay scans the same transactions and rewrites the same bytes. A crash during
R5's journal-superblock write tears at most one slot, and the other slot still describes the old
tail, which again replays identically. Replaying any number of times gives the same image as
replaying once (invariant I9).

**Torn and reordered writes.** Before a transaction's commit block is written, all its other
blocks are durable (F1). So after a crash either the commit block is complete and valid and the
whole transaction is on disk, or the commit block is missing, stale (wrong `seq`/`logPos`) or torn
(bad checksum) and the transaction is ignored. The per-tag `dataCsum` catches media corruption of
committed transactions (R3 step 5).

---

## 18. Mount, unmount, and runtime errors

### 18.1 Mount

1. **M1** Read block 1 and validate the superblock (§4.2). (It is safe to trust it before replay:
   its fields sit in one sector, so a torn checkpoint write of block 1 leaves either the old or
   the new version, both valid.)
2. **M2** Apply the feature rules and the mount decision table (§5.3).
3. **M3** Replay (§17). Nothing else is validated before this step: a checkpoint write torn by the
   crash can leave any other metadata block invalid until replay rewrites it.
   If replay applied anything, **re-read and re-validate block 1**: replay may have rewritten the
   superblock (for example `orphanHead`), and every later superblock write must start from the
   replayed version, never from the copy read in M1.
4. **M4** Read every GDT block and check every descriptor: checksum, and the three location
   fields equal their derived values. Counters must be within range (`freeBlocks` <= blocks in
   the group, `freeInodes` and `inodesInitialized` <= `inodesPerGroup`). A failure refuses rw
   (`STATUS_ERR_CORRUPT`); ro continues with locations from geometry.
5. **M5** Read-write only. If `ERRORS` is set (already, or because replay requires it): write the
   superblock as in R6 (`NEEDS_RECOVERY` cleared, `ERRORS` set), FLUSH, and fail with
   `STATUS_ERR_READ_ONLY` (the caller may retry read-only). Otherwise set `NEEDS_RECOVERY`,
   `mountCount += 1`, `lastMountTimeNs = now`, write the superblock directly, FLUSH (F6). The
   journal is empty at this point, so a direct write is safe.
6. **M6** In memory: total free blocks and inodes = sums of the descriptor counters; per-group
   summaries for the allocator.
7. **M7** Read-write only: orphan recovery (§12.2).
8. **M8** Ready.

Mount memory: the superblock, the GDT (`gdtBlocks * 4096` bytes if cached whole; at most 32 MiB at
64 TiB, 512 KiB at 1 TiB; an implementation MAY cache only a per-group summary of about 16 bytes
per group plus GDT blocks on demand), the replay buffers (§17), and the orphan walk's tree paths
(fixed-size arrays).

### 18.2 Clean unmount (read-write)

1. **U1** Commit the running transaction and wait.
2. **U2** Checkpoint everything (§16.6), so the tail reaches the head (K4 writes the journal
   superblock, with FLUSH).
3. **U3** Write the superblock directly: `NEEDS_RECOVERY` cleared, `freeBlocksHint` and
   `freeInodesHint` = the sums of the descriptor counters, `lastWriteTimeNs = now`. FLUSH (F7).

### 18.3 Runtime errors

A metadata block that fails validation (checksum, magic, binding fields, structural rules) when
read makes the current operation fail with `STATUS_ERR_CORRUPT`; an I/O error, `STATUS_ERR_IO`.
Then, if the filesystem is mounted read-write and either (a) corruption was detected or (b) a
write inside the journal machinery failed:
1. **Abort** the journal: the running transaction is discarded (never committed); new handles
   fail with `STATUS_ERR_READ_ONLY`. Already-committed transactions stay in the log. After an
   abort nothing is checkpointed or committed any more (a later checkpoint would write superblock
   copies without `ERRORS` over the one written in step 2); the next mount's replay finishes the
   committed work.
2. Write the journal superblock (other slot, `updateCount + 1`, same tail, `fsErrorFlags` bit 0
   set), FLUSH; then write the superblock directly with `ERRORS` set, FLUSH (F11). The journal
   copy of the flag is what survives a later replay, which would otherwise restore superblock
   copies logged before the error. The superblock written here MUST be the newest **committed**
   version of block 1 (the newest frozen copy on the checkpoint list, else the on-disk block) with
   only `ERRORS` added, never the in-memory version, which may hold uncommitted changes of the
   discarded transaction (for example an `orphanHead` whose inode change was never committed).
3. Apply `errorBehavior`: 1 = the mount becomes read-only (reads of intact data keep working),
   2 = kernel panic.

On a read-only mount nothing is written: the operation fails and the error is logged.

A read error on a file data block fails only that read (`STATUS_ERR_IO`) and doesn't mark the
filesystem.

---

## 19. mkfs.bongfs and bongfs-cp

### 19.1 mkfs.bongfs

`mkfs.bongfs [options] <device-or-image>`

| Option | Meaning |
|---|---|
| `-s <blocks>` | filesystem size in 4 KiB blocks (an image file is created or extended to that size); default: the whole device or file |
| `-L <label>` | label, 0..63 bytes of UTF-8 |
| `-U <uuid>` | UUID (canonical text form); default random (RFC 4122 v4) |
| `-i <bytes>` | bytes per inode, a power of two in 4096..1048576 (default 16384) |
| `-J <blocks>` | journal blocks, 1024..16384 (default per §3.4) |
| `-m <percent>` | reserved percentage, 0..50 (default 5; capped at 262144 blocks) |
| `-e ro\|panic` | `errorBehavior` (default `ro`) |
| `-T <ns>` | timestamp for `createTimeNs` and the root/`lost+found` inodes; default: `SOURCE_DATE_EPOCH` (seconds) if set, else now |
| `--deterministic <u64>` | derive UUID, `dirHashKey` and the first `tailSeq` from the seed (below) instead of random bytes |
| `-F` | overwrite a device or image that already holds a bongfs superblock (default: refuse, exit 1) |
| `-n` | dry run: print the geometry (the fields of §3.5) and write nothing |

**Deterministic mode.** Let `K = le64(seed) || le64(seed XOR 0xFFFFFFFFFFFFFFFF)` (a 16-byte
SipHash key) and `H(s) = SipHash-2-4(K, ASCII s)`. Then `uuid = le64(H("uuid0")) ||
le64(H("uuid1"))` with byte 6 = `(byte6 & 0x0F) | 0x40` and byte 8 = `(byte8 & 0x3F) | 0x80`
(RFC 4122 v4 bits); `dirHashKey = le64(H("hash0")) || le64(H("hash1"))`; first
`tailSeq = (H("seq") & 0xFFFFFFFF) + 1`. Random mode takes the same three values from the host
CSPRNG (`getrandom`).

Worked example (host tests MUST check it), seed 1: `uuid` bytes
`1a 62 c2 0e da 06 4e 54 84 f5 db 0d fe e1 65 3c`, `dirHashKey` bytes
`61 44 aa 9f 65 4a 57 fa 5e a6 fc 4b 14 72 16 0a`, first `tailSeq` 3148697273.

**Procedure** (the primary superblock is written last, so an interrupted mkfs leaves nothing
mountable):
1. Compute and check the geometry (§3.4). With `-n`, print it and stop.
2. Write block 0 as zeros. Write the whole journal area as zeros.
3. For each group, write its block bitmap (fixed metadata and past-end bits 1) and inode bitmap
   (padding bits 1; in group 0 bits 0..15 set: inodes 1..15 and `lost+found`).
4. Write group 0's first inode-table block (inodes 1..16): 1 and 3..15 in the free form with
   generation 0; the root; `lost+found`. No other inode-table block is written.
   - Root (2): mode `0x41ED` (directory, 0755), uid 0, gid 0, `linkCount` 3, generation 1,
     `INLINE`, `size` 112, `allocatedBlocks` 0, all four timestamps = the mkfs time. Inline
     directory: `parentIno` 2, one entry `lost+found` -> 16, `fileType` 2.
   - `lost+found` (16): mode `0x41C0` (directory, 0700), uid 0, gid 0, `linkCount` 2,
     generation 1, `INLINE`, `size` 112, empty inline directory with `parentIno` 2.
5. Write the GDT: locations per §3.2, counters from the bitmaps (group 0: `usedDirs` 2,
   `inodesInitialized` 16; every other group: `usedDirs` 0, `inodesInitialized` 0), bitmap
   checksums.
6. Write journal superblock slot 0: `updateCount` 1, `tailPos` 2, `tailSeq` = the first
   sequence number, `fsErrorFlags` 0. Slot 1 stays zero (invalid).
7. Write every backup superblock (§4.3).
8. FLUSH. Write the primary superblock (`NEEDS_RECOVERY` clear, hints = the counter sums,
   `mountCount` 0, `lastMountTimeNs`/`lastWriteTimeNs`/`lastCheckTimeNs` 0). FLUSH.

Exit codes: 0 success, 1 failure (message on stderr), 16 usage error.

### 19.2 bongfs-cp

`bongfs-cp [options] <image> <host-dir> [<dest-dir>]` copies the contents of `host-dir` into
`dest-dir` (default `/`, which must already exist) of the image. It mounts the image read-write
through `libs/bongfs` (journal and all) and unmounts cleanly at the end, so its output is always
fsck-clean, and after a failure the image is still consistent (partially populated).

- **Order:** directory entries are copied in byte-wise sorted name order, depth first, so the
  image is a pure function of the source tree, the options, and the image's starting state.
- **Owners:** `--owner uid:gid` (default `0:0`) for everything; `--preserve-owner` keeps the host
  values.
- **Times:** all four timestamps = the source `mtime`, clamped to `SOURCE_DATE_EPOCH` when that is
  set (reproducible builds).
- **Modes:** permission bits copied as is, including setuid/setgid/sticky.
- **Hard links:** sources with the same `(st_dev, st_ino)` become links to one inode.
- **Symlinks:** the target bytes verbatim.
- **Sparse files:** a 4 KiB-aligned all-zero block of the source becomes a hole.
- **Specials** (devices, FIFOs, sockets): copied only with `--specials`, else skipped with a
  warning.
- **Xattrs:** copied only with `--xattrs` (namespaces per §9.1).
- A name longer than 255 bytes, a symlink target longer than 4095 bytes, or running out of space
  stops the copy with exit 1.

Exit codes: 0 success, 1 failure, 16 usage error.

---

## 20. Limits

| Item | Limit | Where it comes from |
|---|---|---|
| Block size | 4096 bytes | fixed in v1 |
| Filesystem size | 16 MiB (4096 blocks) .. 64 TiB (2^34 blocks) | group 0 must hold the GDT for 2^19 groups plus the journal |
| Groups | 1 .. 524288 | as above |
| Inodes per group | 64 .. 32768, a multiple of 16 | one inode-bitmap block |
| Inodes | `groupCount * inodesPerGroup`, at most 2^34 | inode numbers are `u64` on disk |
| File size | 2^44 bytes (16 TiB) | `u32` logical block numbers |
| Extent length | 32768 blocks (128 MiB) | `u16` length; in practice a run can't exceed one group's data area |
| Extent tree depth | 4 (root in the inode + up to 4 levels of blocks) | §8.4 |
| Name length | 255 bytes | `u8 nameLen` |
| Symlink target | 4095 bytes (112 inline) | one block; `PATH_MAX` 4096 |
| Links per inode | `0x7FFFFFFF` | `u32 linkCount`, kept positive as a signed value |
| Subdirectories per directory | `0x7FFFFFFF - 2` | `linkCount` |
| Entries per directory | see §10.7 "Capacity" (tens of millions); logical blocks < 2^32 | node levels <= 4 |
| Xattrs per inode | 48 bytes inline + 4064 bytes in one block (entry headers and names included) | §9 |
| Single xattr value | `4064 - 4 - nameLen` bytes | one block |
| Journal | 1024 .. 16384 blocks (4 .. 64 MiB) | §3.4 |
| Transaction | `journalBlocks / 4` log blocks | §15.1 |
| Timestamps | years 1677 .. 2262 | `s64` nanoseconds |
| uid, gid | 32-bit | |
| Label | 63 bytes | |

---

## 21. Crash-consistency invariants

### 21.1 Crash model

The crash tester records the sequence of block writes and FLUSHes the library issues to its
device. A **crash point** `w` is a write boundary: the moment after the `w`-th write was issued.
The crashed image is:
- the image as of the last FLUSH that **completed** before `w`, plus
- an arbitrary subset (chosen by the tester's seeded RNG) of the writes issued after that FLUSH
  and before `w`, applied in issue order.

Two granularities: **block mode** (each write lands whole or not at all) and **sector mode**
(each 512-byte sector of each write lands independently). The design is correct in both (§4.1,
§15.2, §17); M7.6 MUST test block mode and SHOULD test sector mode.

**Logical state** of a filesystem: the namespace tree (names, file types, which entries share an
inode), and for every inode `mode`, `uid`, `gid`, `linkCount`, `size`, `mtimeNs`, `ctimeNs`,
`btimeNs`, the `IMMUTABLE`/`APPEND`/`NOATIME` flags, the xattr set, the symlink target, the device
numbers, and the file contents. `atimeNs` is excluded.

Let `S(k)` be the model's logical state after transaction `k` committed (the library reports each
commit's sequence number and the operations it contains through a test hook, §25). Let `d` be the
last transaction whose C10 FLUSH completed before `w`.

### 21.2 Invariants

For every crash point, in both granularities:

- **I1** `fsck.bongfs -n` on the crashed image (replaying the journal into its in-memory overlay)
  exits 0.
- **I2** After a read-write mount (replay + orphan recovery) and a clean unmount,
  `fsck.bongfs -n` exits 0; the superblock has `NEEDS_RECOVERY` clear, `orphanHead == 0`, and hints
  equal to the descriptor sums; a journal scan finds no transaction.
- **I3** The recovered logical state (file contents per I5) equals `S(d)` or `S(d + 1)`. `S(d + 1)`
  is possible only if `d + 1`'s commit block write was issued before `w`. (Commits are
  serialized, §16.4, so no other transaction can be in doubt.)
- **I4** Durability: a file whose `fsync` returned before `w` has, after recovery, at least the
  size and data it had at that `fsync` (unless a later operation in a transaction `<= d` changed
  them).
- **I5** Ordered data: for every regular file in the recovered state, each byte in `[0, size)` is
  either 0 or the value some write in the model's history put at that offset of that file (the
  tester pre-fills the whole device with a canary pattern before mkfs; the canary never appears
  in a file). For a block that the recovered state maps as written in transaction `d` or earlier
  and that was never overwritten in place, the content equals the model's exactly.
- **I6** Block ownership: every block referenced by an extent, directory, xattr or tree pointer is
  inside a data area, referenced exactly once, and marked used in its bitmap; every fixed-metadata
  block and bitmap padding bit is 1; every other bit is 0.
- **I7** Counters and checksums: every descriptor's counters equal its bitmaps; every metadata
  checksum and binding field (§2.3) is valid.
- **I8** Reachability: every allocated inode except the reserved ones is reachable from the root
  or is on the orphan list; every directory entry targets an allocated inode of its `fileType`;
  every `linkCount` is correct (§12.1); every directory's `parentIno` names the directory holding
  its entry.
- **I9** Replay idempotence: crashing at any write boundary **during replay** and then replaying
  again yields an image identical, in every block outside the journal-superblock slots (whose
  `tailSeq`/`tailPos` must also match), to one uninterrupted replay.
- **I10** `fsck.bongfs -n` never writes: the image is byte-identical before and after.
- **I11** Atomicity: an operation that returned an error left the logical state unchanged.
- **I12** Determinism: `mkfs.bongfs --deterministic <seed>` with the same arguments produces
  byte-identical images; `bongfs-cp` with fixed `SOURCE_DATE_EPOCH` does too; every such image
  passes `fsck.bongfs -n` with exit 0.
- **I13** Repair convergence (corruption-injection tests): after `fsck.bongfs -y`, a second
  `fsck.bongfs -n` exits 0.

---

## 22. fsck.bongfs

### 22.1 Interface

`fsck.bongfs (-n | -p | -y) [-b <group>] [-v] [--machine] <device-or-image>`

| Mode | Meaning |
|---|---|
| `-n` | **read-only**: opens the device read-only and never writes (I10). A journal that needs replay is replayed into an in-memory overlay (bounded by the journal size, at most 64 MiB). Every problem is reported, none fixed |
| `-p` | **preen**: fixes the problems classed AUTO; at the first problem classed ASK it stops, reports, and exits with 4 set |
| `-y` | fixes everything it can (AUTO and ASK) |
| `-b <group>` | use the backup superblock in that group instead of the primary |
| `-v` | verbose |
| `--machine` | one line per problem, `PROBLEM <check> <class> <key>=<value>...`, then `RESULT exit=<n> fixed=<a> remaining=<b>` (the M7.6 tests parse this) |

Exactly one of `-n`/`-p`/`-y` is required. fsck always performs the full check (there is no
"clean, skipping" shortcut). It refuses to run on a filesystem that is mounted (host: an
exclusive open of the device or image).

**Exit code** = the OR of: 0 no problems; 1 problems were found and all fixed; 4 problems remain
(always the case for problems found with `-n`); 8 operational error (I/O error, unknown incompat
bit, repair refused because of an unknown feature bit, no valid superblock); 16 usage error.
Value 2 (e2fsck's "reboot needed") is never used by v1.

**Classes:** NOTE = not a problem (no effect on the exit code); AUTO = safe to fix unattended
(no user data can be lost: rebuilding derived data, reconnecting, finishing interrupted
operations); ASK = the fix may discard user data or relies on a guess; FATAL = cannot be repaired
(exit 4, or 8 where stated).

**Writes.** Repairs are written directly (not through the journal). Before the first repair
fsck sets `ERRORS` in the superblock and FLUSHes; after the last it FLUSHes, then (if nothing
remains) clears `ERRORS`, sets `lastCheckTimeNs`, rewrites the superblock and any stale backup,
and FLUSHes (F10). An interrupted repair therefore leaves the filesystem flagged, and running
fsck again is always safe.

**Memory** (host only; the kernel never runs fsck): two bitmaps of `blockCount / 8` bytes each
(blocks seen, blocks seen twice), about 32 bytes per allocated inode, about 16 bytes per
directory, the journal overlay with `-n`, and fixed-depth path arrays. No recursion (explicit
stacks: extent depth <= 4, directory levels <= 4, directory hierarchy walked with an explicit
queue).

### 22.2 Checks

**Pass 0: superblock, journal, orphans**

| # | Check | Class | Repair |
|---|---|---|---|
| F0.1 | primary superblock magic, version, checksum, geometry (§4.2) | ASK | take the first valid backup in group order 1, 3, 5, 7, 9, 25, 27, 49, ... (or `-b`) whose geometry is self-consistent; restore static fields from it, dynamic fields to 0; the journal and orphan list are then found by the scan and pass 4. No valid copy: FATAL (8) |
| F0.2 | unknown feature bits | FATAL (8) for incompat; for compat/roCompat only `-n` may continue | none |
| F0.3 | static-field rules: label format, `errorBehavior`, `reservedBlocks`, unknown `stateFlags` bits, reserved fields and bytes 512..4095 zero | AUTO | reset to the defaults of §19.1 / zero |
| F0.4 | journal superblock slots (§17 R1) | ASK if neither is valid | write a fresh slot: `tailPos` 2, `tailSeq` = 1 + the largest `seq` seen in any valid journal header in the log (or 1), zero the log. Committed but unreplayed transactions are lost, so the later passes will find and fix what they left |
| F0.5 | journal replay needed | NOTE | replay (§17): on disk with `-p`/`-y`, into the overlay with `-n` |
| F0.6 | replay stopped at a committed but damaged transaction (R3 step 5) | ASK | nothing more can be recovered from the log; the fs is consistent as of the previous transaction; the remaining passes verify it |
| F0.7 | orphan list: every member has `ORPHAN`, list length <= `inodeCount`, no cycle, `orphanNext` 0 outside the list | AUTO | rebuild the list from the inodes that have `ORPHAN` set (found in pass 1) |
| F0.8 | orphans present | NOTE | `-p`/`-y`: recover them (§12.2) after pass 1 validated each one; `-n`: validate only |

**Pass 1: group descriptors and inodes**

| # | Check | Class | Repair |
|---|---|---|---|
| F1.1 | GD checksum; location fields equal §3.2 | AUTO | rewrite locations from geometry; counters are rebuilt in pass 5 |
| F1.2 | `inodesInitialized` <= `inodesPerGroup` | AUTO | treat as `inodesPerGroup` while scanning (every slot is then examined); set properly in pass 5 |
| F1.3 | reserved inodes 1, 3..15 in the free form | AUTO | rewrite them |
| F1.4 | each slot with its bitmap bit set: checksum | ASK | if every other check of this pass passes, recompute the checksum; otherwise clear the inode (its entries are removed in pass 2) |
| F1.5 | mode type valid; permission bits; unknown `flags` bits | ASK (type) / AUTO (flags) | clear the inode / clear the unknown bits |
| F1.6 | per-type rules (§7.8): `INLINE` vs type and size, `size` range, `iData` format, device-node padding | ASK | clear the inode (its entries are removed in pass 2) |
| F1.7 | extent tree (§8.3-§8.4): every node's magic, checksum, `selfBlock`, owner, generation, `depth`, `entries`, `maxEntries`, key order and ranges, extent lengths and flags, physical runs inside data areas | ASK | cut the tree at the first bad node (drop that subtree), then fix `size`/`allocatedBlocks` |
| F1.8 | written extents past EOF without `TRUNCATING` (§8.4 invariant 6) | ASK | free them (they are invisible to reads) |
| F1.9 | `TRUNCATING` without `ORPHAN` | AUTO | put the inode on the orphan list (recovery finishes the truncate) |
| F1.10 | xattrs: inline area parses to exactly `xattrInlineUsed`, sorted, unique, valid namespaces; overflow block magic, checksum, binding, parse | ASK | drop the bad entries or the bad block |
| F1.11 | `allocatedBlocks` equals the recount | AUTO | set it |
| F1.12 | every block the inode references is recorded in the "seen" bitmap; a block seen twice goes to the "dup" bitmap | - | handled in pass 1b |
| F1.13 | a slot below `inodesInitialized` whose bitmap bit is clear and which is not in the free form | NOTE if it looks live (valid checksum, `mode != 0`, `linkCount > 0`): remembered as **possibly lost** for F2.5/F4.3; AUTO otherwise | rewrite it in the free form (never a possibly-lost one) |

**Pass 1b: duplicate blocks** (only if the dup bitmap is non-empty)

| # | Check | Class | Repair |
|---|---|---|---|
| F1b.1 | a block referenced more than once (between files, or overlapping fixed metadata) | ASK | rescan to find every owner; a block overlapping fixed metadata is dropped from the file; for data blocks every owner but the lowest inode number gets a fresh copy (clone); for tree/directory/xattr blocks the higher-numbered owners lose that subtree |

**Pass 2: directories**

| # | Check | Class | Repair |
|---|---|---|---|
| F2.1 | inline directory header: `usedBytes` <= 96, entries parse exactly, `entryCount` matches | ASK | rebuild the directory from the entries that parse |
| F2.2 | hashed directory nodes: magic, checksum, `ownerIno`, `ownerGeneration`, `logicalBlock`, levels, `count`, `usedBytes`, index keys and ranges (§10.6), each in-use node reachable once, free list well formed, in-use + free = all logical blocks | ASK for unreadable or misplaced nodes; AUTO for free-list and unreferenced-node problems | rebuild the directory: collect every valid entry from the readable leaves and write a fresh tree (AUTO when no entry was lost, ASK otherwise); unreferenced nodes go on the free list |
| F2.3 | each entry: name rules (§10.1), stored hash equals SipHash, `(hash, name)` order, hash inside its leaf's range | AUTO for hash/order (rebuild the directory, nothing lost); ASK for a bad name (remove the entry) | as stated |
| F2.4 | duplicate names in one directory | ASK | keep the first in order, remove the others (their inodes may reach `lost+found` in pass 4) |
| F2.5 | target inode number in `16..inodeCount` and allocated and valid after pass 1; a "possibly lost" target counts as allocated | ASK (invalid target) / AUTO (possibly-lost target) | remove the entry / mark the target allocated |
| F2.6 | `fileType` matches the target's mode | AUTO | set it |
| F2.7 | a directory named by more than one entry | ASK | keep the entry in its recorded parent (`parentIno`), remove the others |

**Pass 3: connectivity**

| # | Check | Class | Repair |
|---|---|---|---|
| F3.1 | the root inode exists, is a directory, `parentIno == 2` | ASK if missing or not a directory; AUTO for `parentIno` | recreate an empty root (everything else is reconnected by F3.2) / set it |
| F3.2 | every directory is reachable from the root (walk entries from the root; directories on cycles are unreachable) | AUTO | reconnect it into `/lost+found` as `#<ino>` (decimal); create `/lost+found` (mode 0700, inline) if missing |
| F3.3 | each directory's `parentIno` equals the directory holding its entry | AUTO | set it |

**Pass 4: link counts**

| # | Check | Class | Repair |
|---|---|---|---|
| F4.1 | `linkCount` equals the counted references (§12.1) | AUTO | set it |
| F4.2 | an allocated, non-orphan inode with no references | AUTO | size 0 and no xattrs: free it; otherwise reconnect it into `/lost+found` as `#<ino>` |
| F4.3 | a "possibly lost" inode (F1.13) with no references | ASK | `-y`: mark it allocated and reconnect it into `/lost+found`; `-p` stops here |
| F4.4 | an orphan-list member with `linkCount > 0` and no `TRUNCATING` | AUTO | remove it from the list |

**Pass 5: bitmaps, counters, hints, backups**

| # | Check | Class | Repair |
|---|---|---|---|
| F5.1 | block bitmaps equal the computed usage (fixed metadata, padding bits, every referenced block) | AUTO | rewrite the bitmaps |
| F5.2 | inode bitmaps equal the allocated inodes (reserved inodes included, padding bits set) | AUTO | rewrite them |
| F5.3 | descriptor counters, `usedDirs`, bitmap checksums; `inodesInitialized` >= the highest allocated index + 1 | AUTO | set them (raising `inodesInitialized` if needed; lowering it is never required) |
| F5.4 | superblock hints equal the sums (checked only when `NEEDS_RECOVERY` was clear at the start) | AUTO | set them |
| F5.5 | every backup superblock valid and equal to the primary's backup image (§4.3) | AUTO | rewrite it |

A crashed image (after replay, before orphan recovery) is **clean**: orphans, the
`NEEDS_RECOVERY` bit, stale hints and pinned-free blocks leave no trace that any check above
reports as a problem. That is what makes I1 testable.

---

## 23. Memory, portability, and `HOSTED` rules

`libs/bongfs` is one body of portable C17 built three ways: into the kernel, into the host tools
(`HOSTED` defined), and into the host tests (ASan/UBSan, D-054).

- **No floating point, no VLAs, no recursion.** Tree walks use fixed arrays sized by the depth
  limits (extent path <= 5 nodes, directory path <= 4 nodes); fsck's hierarchy walk uses an
  explicit heap-allocated queue.
- **No struct overlays on disk bytes.** Encode and decode with little-endian byte helpers; the
  in-memory structs are separate from the on-disk layout.
- **No libc or kernel headers in the core.** The library only includes the freestanding headers of
  ARCHITECTURE §4 plus its own. Everything else comes through an environment table the caller
  supplies: block I/O (`read`, `write` of whole 4 KiB blocks, `flush`, block count, and whether
  the device is writable), memory (`alloc`/`free`), the clock (`nowNs`), logging, locks (no-ops on
  the host), and random bytes (mkfs only). The kernel's table wraps `BlockDevice` `submitIo`/`flush`
  (synchronously for the library's purposes) and `kmalloc`.
- **Allocation-failure safe.** Every allocation can fail; failure returns `STATUS_ERR_NO_MEMORY`
  before any metadata modification (§16.2), or, if it happens inside the commit machinery, aborts
  the journal (§18.3).

**Memory bounds** (4 KiB block buffers counted as "buffers"):

| Activity | Bound |
|---|---|
| Replay | 3 buffers, O(1) otherwise |
| Mount | superblock + GDT (cached whole, `gdtBlocks` buffers, or a 16-byte-per-group summary plus on-demand GDT buffers) + replay |
| One operation | its credit count in buffers (at most 192, typically under 20) + fixed path arrays |
| Running + committing transaction | at most `2 * maxTxnBlocks` buffers of frozen copies + the checkpoint list (<= `journalBlocks` entries) + the pinned-block sets (§13.5) |
| Orphan recovery | one inode + one extent path at a time |
| fsck (host only) | §22.1 |

The kernel's buffer cache may hold more for performance, but correctness never requires more than
the table above.

---

## 24. Kernel driver notes (M7.7)

Not on-disk format, but constraints the format relies on:

- Every bongfs entry point **may sleep**; none is IRQ-safe or callable with a spinlock held.
- **Lock order:** (1) the per-mount rename lock (cross-directory renames only); (2) vnode locks,
  parent before child, otherwise ascending inode number; (3) starting a journal handle (which may
  wait for a commit or checkpoint); (4) per-group allocator locks in ascending group order; (5)
  the journal's internal state lock (a leaf). The commit thread takes only (4)-(5) and I/O waits;
  it never takes a vnode lock or a page lock that a handle holder could be holding while it waits
  for that same commit.
- One commit thread per mount runs §16.4 and, when needed, §16.6.
- Metadata goes through a block buffer cache keyed by block number (with the frozen-copy
  mechanism of §16.4 C4); file data goes through the page cache, and its writeback feeds the
  ordered lists (§16.5).
- All writes go through the block layer, so the real-disk write guard (ROADMAP safety rule) still
  applies to bongfs.
- `fsync` and `fsSync` follow §16.7 exactly (M7.3's `fsync` contract).

---

## 25. Test hooks for M7.6

What the spec guarantees that the fuzzer and the crash tester can rely on:

1. **Injectable device.** The environment table (§23) lets tests supply a RAM device that records
   every write and FLUSH in order (the crash model of §21.1 is built from that log).
2. **Deterministic images.** `mkfs.bongfs --deterministic <seed>` plus an injected clock gives
   byte-identical images; allocation is deterministic (§13); so a failing fuzz seed reproduces
   exactly.
3. **Commit visibility.** The library exposes `bfsCommit()` (synchronous commit of the running
   transaction) and a hook called at C11 with the transaction's `seq` and its operations, so the
   tester can snapshot the model state `S(k)` at each commit. A test configuration that commits
   after every operation is allowed (and is the simplest for the crash tester).
4. **At most one transaction in doubt** at any crash point (§16.4), so I3 has exactly two
   acceptable outcomes.
5. **Standalone replay.** `bfsJournalReplay(env)` runs §17 without mounting, so I9 can crash
   replay itself.
6. **Checker API.** `bfsCheck(env, mode, report callback)` is the fsck engine, callable in-process
   with the check codes of §22.2, so tests assert on codes instead of parsing text (the
   `--machine` output is equivalent).
7. **Canary data.** mkfs never writes data areas except the blocks it allocates, so a device
   pre-filled with a canary pattern keeps it everywhere else (I5).
8. **Known answers.** CRC32C (§2.1), `metaCsum` (§2.2), and SipHash-2-4 (§10.2) vectors, plus the
   worked geometry of §3.5 (`mkfs.bongfs -n` output), are fixed test data.
9. **Size.** The minimum filesystem (16 MiB, 1024-block journal, 256-block transactions) exercises
   every structure: inline-to-hashed directory conversion, tree push-down and splits (with small
   files and many 1-block extents), journal wrap-around (the log is only 1022 blocks), checkpoint,
   and orphan recovery. 10k fuzz iterations and 1k crash points on it are cheap.
10. **Model scope.** The model compares the logical state of §21.1 (no `atime`, no physical
    layout), so allocator refinements don't break the model tests; physical layout is pinned only
    by the determinism test (I12).

---

## 26. Proposed DECISIONS.md rows

All of these define or constrain the on-disk format, so all need the owner (D-045). Numbers get
assigned when they are appended.

| ID | Decision | Rejected | Why |
|---|---|---|---|
| D-0xx.1 | bongfs geometry: fixed 4 KiB blocks, 32768-block (128 MiB) groups with bitmaps and inode table at each group's start, one GDT after the primary superblock with no GDT backups (every location is derivable from the superblock), superblock backups in groups 1 and powers of 3/5/7, block 0 a reserved zeroed boot area, v1 capped at 2^19 groups (64 TiB) | flex_bg-style packed metadata; per-group self-describing headers; GDT backups; variable block size | the simplest layout in which fsck can recompute every location from the superblock alone; 64 TiB is far beyond any target disk |
| D-0xx.2 | Directly written structures (superblock, journal superblock) keep all fields and their checksum in the first 512 bytes of their block; the journal superblock has two slots chosen by `updateCount` | whole-block structures; one journal superblock | a direct write torn at sector granularity leaves the old or new version, never neither; the tail update is atomic |
| D-0xx.3 | Checksums are `CRC32C(uuid, binding, bytes)` with the field zeroed; bitmap checksums live in the group descriptor; tree, directory and xattr blocks carry owner inode + generation + self address; journal copies bind `seq` and target | no bitmap checksums; unbound checksums; a stored seed | catches misdirected and stale blocks, not just bit rot; the UUID never changes in v1, so no stored seed is needed |
| D-0xx.4 | Inodes: 256 bytes, `u64` inode numbers, reserved 1..15, root 2, `lost+found` 16 at mkfs, lowest-free-slot allocation with a per-group high-water mark (`inodesInitialized`), inode tables not zeroed by mkfs, per-slot generation | `u32` inode numbers; zeroing inode tables; ext4-style uninit flags | 64 TiB at default density exceeds 2^32 inodes; mkfs stays fast; fsck knows exactly which slots to read |
| D-0xx.5 | Extents: 16-byte entries (`u32` logical, `u16` length <= 32768, `UNWRITTEN` flag, `u64` physical), 6-entry root in the inode, 254-entry blocks, depth <= 4, index entry 0 stored as key 0, midpoint splits, only empty nodes freed plus root collapse | ext4's 12-byte extents with 48-bit physical numbers; 64-bit logical numbers | 64-bit block numbers (§15.1) at 16 bytes per extent; 16 TiB files are plenty; invariants simple enough for fsck to check exactly |
| D-0xx.6 | Directory hash = SipHash-2-4 keyed by a random per-filesystem 128-bit `dirHashKey`; order `(hash, name)`; equal hashes must share a leaf (else `STATUS_ERR_NO_SPACE`) | seeded FNV/xxHash; ext4 half-MD4/TEA; collision chains across leaves | resists hash flooding in shared directories, which is what makes the one-leaf-per-hash rule safe; public reference vectors |
| D-0xx.7 | Directories: inline in the 112-byte `iData` first, then a hashed B+tree in the directory's own logical blocks with the root at logical 0, a per-directory free-node list, no shrinking, leaf merges below 1/4 when the result fits in 3/4, index nodes freed only when empty, no conversion back to inline, `..` stored as `parentIno` | linear directory blocks; a tree of physical pointers; freeing nodes back to the allocator | removals never allocate (bounded credits, delete works on a full disk); fsck can account for every directory block |
| D-0xx.8 | Xattrs: 48-byte inline area + at most one unshared overflow block; namespaces user/trusted/security/system | ext4 shared refcounted xattr blocks; a movable inline split | matches §15.1 with no refcounting |
| D-0xx.9 | Journal: a contiguous area in group 0 with no inode; physical block logging (254 tags per descriptor); commit = FLUSH, commit block, FLUSH; commits strictly serialized; `maxTxnBlocks = journalBlocks / 4`; size `clamp(blockCount/64, 1024, 16384)` rounded to 256; **no revoke records** (the allocator pins freed blocks from data reuse until checkpoint) and no escaping; replay on every mount with O(1) memory, bumping `seq` past a torn transaction | a journal inode; async (single-flush) commit; revoke records; logical journaling | the simplest correct ordered-mode protocol; bounded replay memory; at most one transaction in doubt, which the crash tests rely on |
| D-0xx.10 | Ordered mode precisely: newly mapped/converted blocks, the old-EOF block on size increase, and long-symlink blocks are written before the commit; the partial-tail bytes past EOF are unspecified on disk and zeroed on every size increase; replace-by-rename orders the source file's data | data=journal; ordering only newly allocated blocks; no rename rule | never exposes stale data, and makes the common "write temp, rename over" save pattern crash-safe |
| D-0xx.11 | Orphan list (superblock head, `orphanNext` chain, `ORPHAN`/`TRUNCATING` flags); deletes and large truncates freed in chunks of 8 extents per transaction | ext4's orphan file; one-transaction deletes | every transaction stays bounded while unbounded operations stay crash-safe |
| D-0xx.12 | Free space: bitmaps authoritative, descriptor counters journaled with them, superblock counts are hints written only at clean unmount; reserve = `min(5 %, 1 GiB)` for `reservedUid`/`reservedGid` | journaled superblock counters; an uncapped 5 % | no superblock write per allocation; a sane reserve on large disks |
| D-0xx.13 | Allocation is deterministic and the reference algorithm of §13 is normative (top-level directories spread by name hash, files near the parent, first-fit runs from the goal, halving the run length on failure) | randomized Orlov; mballoc-style buddy preallocation | reproducible fuzz failures and reproducible images |
| D-0xx.14 | Error policy: runtime corruption aborts the journal, sets `ERRORS` in the superblock and `fsErrorFlags` in the journal superblock, then `errorBehavior` (read-only by default, or panic); with `ERRORS` set a read-write mount is refused until fsck; unknown roCompat bits allow only read-only mounts | ext4's "continue" and read-write mounts with errors | conservative for a new filesystem (see Q3) |
| D-0xx.15 | fsck: `-n`/`-p`/`-y` required, full check always, exit bits 0/1/4/8/16 (2 unused), NOTE/AUTO/ASK/FATAL classes, `--machine` output, direct writes bracketed by the `ERRORS` flag; mkfs `--deterministic` seed and `SOURCE_DATE_EPOCH` | an interactive default; journaled repairs | automatable tests; an interrupted repair is always safe to rerun |
| D-0xx.16 | Naming: C prefix `bfs`; superblock magic `0x0A1A0A0D5346427F`; block magics `XT`, `DIRN`, `XATB`, `JRNL`; discard is a mount option, not a feature bit | the OS name in identifiers or magic | D-046; discard changes no on-disk structure |

---

## 27. Deviations from ARCHITECTURE §15.1

No decision is contradicted; five refinements need wording changes in the same PR:

1. **Group descriptors.** §15.1 says each group has "a descriptor, a block bitmap, an inode
   bitmap, and an inode table", which reads as if the descriptor lived inside the group. Here the
   descriptors form one table after the primary superblock (§3.2, §6.1). Proposed text: "**Block
   groups:** 128 MiB each, each with a block bitmap, an inode bitmap, and an inode table; the
   group descriptors form one table after the primary superblock."
2. **Superblock.** Add the backup rule and block 0: "**Superblock:** in block 1 (block 0 is a
   reserved boot area), with backups in group 1 and in groups that are powers of 3, 5, or 7 (3, 5,
   7, 9, 25, 27, 49, and so on)."
3. **Checksums.** The list gains bitmaps and xattr blocks: "**Checksums:** CRC32C, seeded with the
   filesystem UUID, on the superblock, group descriptors, block and inode bitmaps (stored in the
   group descriptor), inodes, directory blocks, extent tree blocks, xattr blocks, and journal
   blocks."
4. **Basics.** Add the v1 limits: "4 KiB blocks, little-endian, 64-bit block numbers; v1
   filesystems up to 64 TiB, files up to 16 TiB."
5. **Journal.** Add: "The log lives in group 0 and has no revoke records; a commit is flush,
   commit block, flush."

ARCHITECTURE §17's primitive table gains a row: "SipHash-2-4 | bongfs directory hashing |
reference vectors" (see Q2 for where the code lives).

---

## 28. Open questions for the owner

Each has a recommendation; the spec above already assumes the recommendation.

- **Q1 Magic number and prefix.** The superblock magic `7F 42 46 53 0D 0A 1A 0A` and the C prefix
  `bfs` keep the OS name out of code (D-046). The magic is permanent once images exist.
  *Recommendation:* accept.
- **Q2 Where SipHash lives, and its label.** It is a keyed PRF used only against hash flooding.
  *Recommendation:* put `sipHash24()` in `libs/crypto` with the reference vectors, list it in
  ARCHITECTURE §17, and state that bongfs relies on it only for denial-of-service resistance. The
  key is readable by anyone with raw read access to the device (root); that is acceptable.
- **Q3 `ERRORS` policy.** With `ERRORS` set, read-write mounts are refused until fsck; a root
  filesystem then comes up read-only with a warning, and until a native `fsck.bongfs` exists in
  the initrd the only fix is running the host tool. The alternative is ext4's behavior (mount
  read-write with a warning). *Recommendation:* refuse read-write, and once native userland exists
  (M8+) ship `fsck.bongfs` in the initrd and run `-p` automatically when `ERRORS` is set.
- **Q4 setuid/setgid.** The format stores the bits. *Recommendation:* the VFS honors them only on
  the root filesystem and mounts everything else `nosuid` by default (M8.7 decides; `elevate` is
  the intended admin path).
- **Q5 Invalid UTF-8 names.** The format accepts any bytes except `/` and NUL.
  *Recommendation:* the native VFS rejects invalid UTF-8 in new names (`STATUS_ERR_INVALID`); the
  Linux personality and `bongfs-cp` pass bytes through (`bongfs-cp` warns).
- **Q6 New `Status` codes.** §1 lists codes that `kernel/include/uapi/status.h` doesn't have yet
  (a user-visible ABI). *Recommendation:* add them with M7.1 (VFS needs most of them anyway) under
  exactly these names.
- **Q7 Limits.** 64 TiB filesystems, 16 TiB files, timestamps to 2262. *Recommendation:* accept;
  lifting them later is an incompat feature.
- **Q8 Discard.** Off by default; it needs a `BlockDevice` discard operation that doesn't exist
  yet; under full-disk encryption it reveals which blocks are free. *Recommendation:* keep it off
  by default, offer a periodic trim tool later, and never enable it automatically on encrypted
  volumes.
- **Q9 `errorBehavior = panic`.** A crafted or damaged filesystem on removable media could panic
  the machine. *Recommendation:* honor `panic` only for the root filesystem; treat it as
  read-only everywhere else.
- **Q10 Needs M7.1.** This spec was drafted before M7.1 (VFS) and M7.3 (page cache). It assumes
  only what ARCHITECTURE §15 states (`BlockDevice` with `flush`, a per-vnode page cache with
  writeback and `fsync`). If those milestones settle on different contracts, §16.5, §16.7 and §24
  may need a revision before M7.7 (not before M7.6, which is host-only).
