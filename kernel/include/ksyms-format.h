/* ksyms v1 wire format (docs/specs/ksyms.md, D-073): the embedded kernel symbol table built by
 * `tools/mksyms` and read at runtime by kernel/core/ksyms-decode.c. This header (and
 * ksyms-decode.c) is deliberately dependency-light -- only stdint/uapi/status.h -- so the exact
 * same decoder builds for the kernel, `tools/mksyms --check`, and tests/host/ksyms_test.c.
 *
 * Layout (all multi-byte fields little-endian, read byte-at-a-time so alignment never matters):
 *   header (32 bytes):
 *     0x00 u32 magic ("KSYM" as bytes, little-endian 0x4D59534B)
 *     0x04 u16 version (1)
 *     0x06 u16 headerSize (32)
 *     0x08 u32 count            -- number of symbols, <= KSYMS_MAX_COUNT
 *     0x0C u32 restartInterval  -- power of two, 1..256
 *     0x10 u32 addrsOff         -- always == headerSize (32)
 *     0x14 u32 restartsOff      -- == addrsOff + 8*count
 *     0x18 u32 namesOff         -- == restartsOff + 4*ceil(count/restartInterval)
 *     0x1C u32 namesSize        -- bytes of name data (padded to a multiple of 8 by the encoder)
 *   addrs[count]: {u32 offset; u32 size}, strictly ascending by offset, offsets relative to
 *     kernelTextStart (not absolute -- keeps the table KASLR-invariant, D-047/D-073)
 *   restarts[ceil(count/restartInterval)]: u32, byte offset *relative to namesOff* of the first
 *     name-stream byte for entry k*restartInterval
 *   names: a stream of front-coded entries, one per symbol in address order: u8 shared, u8
 *     suffixLen, then suffixLen raw bytes. `shared` is the number of leading bytes this name
 *     shares with the previous symbol's name (always 0 at a restart point). No name (shared +
 *     suffixLen) exceeds 255 bytes, and no entry is NUL-terminated in the stream itself. */
#ifndef KERNEL_KSYMS_FORMAT_H
#define KERNEL_KSYMS_FORMAT_H

#include <stdint.h>

#include "uapi/status.h"

#define KSYMS_MAGIC       0x4D59534Bu
#define KSYMS_VERSION     1u
#define KSYMS_HEADER_SIZE 32u
#define KSYMS_MAX_COUNT   (1u << 20)
#define KSYMS_MAX_RESTART 256u
#define KSYMS_NAME_MAX    256u /* including the NUL this decoder adds */

#define KSYMS_OFF_MAGIC            0x00u
#define KSYMS_OFF_VERSION          0x04u
#define KSYMS_OFF_HEADER_SIZE      0x06u
#define KSYMS_OFF_COUNT            0x08u
#define KSYMS_OFF_RESTART_INTERVAL 0x0Cu
#define KSYMS_OFF_ADDRS_OFF        0x10u
#define KSYMS_OFF_RESTARTS_OFF     0x14u
#define KSYMS_OFF_NAMES_OFF        0x18u
#define KSYMS_OFF_NAMES_SIZE       0x1Cu

typedef struct KsymsSymbol {
    char name[KSYMS_NAME_MAX];
    uint64_t offset; /* relative to kernelTextStart */
    uint64_t size;
} KsymsSymbol;

/* Looks up the symbol whose [offset, offset+size) covers `off` (already made relative to
 * kernelTextStart by the caller). Validates the header and every offset/length it reads before
 * trusting it, so a truncated or corrupt blob returns STATUS_ERR_INVALID instead of reading out
 * of bounds; returns STATUS_ERR_NOT_FOUND if no entry covers `off`. Pure, no locks, bounded time
 * (a binary search over `addrs` plus at most restartInterval-1 front-coding decode steps) --
 * reentrant and safe to call from any trap context, including NMI. Host-testable. */
Status ksymsLookup(const uint8_t *blob, uint64_t len, uint64_t off, KsymsSymbol *out);

#endif
