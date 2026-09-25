/* See ksym.h and docs/specs/ksyms.md. Pure and host-testable (no kernel-only dependencies) --
 * ksymSymbolize(), which needs the kernel's own linked-in KSYM blob (kernel.ld's ksymsStart/
 * ksymsEnd) and ksnprintf(), lives separately in ksym-symbolize.c so this file's object can link
 * cleanly into a host test binary with no undefined kernel-linker-symbol references. */
#include "ksym.h"

#include <stdbool.h>

#define KSYM_MAGIC       0x4D59534Bu
#define KSYM_VERSION     1u
#define KSYM_HEADER_SIZE 64u

static bool readU8(const uint8_t *blob, size_t size, size_t off, uint8_t *out) {
    if (off + 1 > size) {
        return false;
    }
    *out = blob[off];
    return true;
}
static bool readU16(const uint8_t *blob, size_t size, size_t off, uint16_t *out) {
    if (off + 2 > size) {
        return false;
    }
    *out = (uint16_t)(blob[off] | ((uint16_t)blob[off + 1] << 8));
    return true;
}
static bool readU32(const uint8_t *blob, size_t size, size_t off, uint32_t *out) {
    if (off + 4 > size) {
        return false;
    }
    *out = (uint32_t)blob[off] | ((uint32_t)blob[off + 1] << 8) | ((uint32_t)blob[off + 2] << 16) |
           ((uint32_t)blob[off + 3] << 24);
    return true;
}
static bool readU64(const uint8_t *blob, size_t size, size_t off, uint64_t *out) {
    uint32_t lo, hi;
    if (!readU32(blob, size, off, &lo) || !readU32(blob, size, off + 4, &hi)) {
        return false;
    }
    *out = (uint64_t)lo | ((uint64_t)hi << 32);
    return true;
}

static bool readUleb128(const uint8_t *blob, size_t size, size_t *ioOff, uint64_t *out) {
    uint64_t result = 0;
    int shift = 0;
    for (;;) {
        uint8_t byte;
        if (!readU8(blob, size, *ioOff, &byte)) {
            return false;
        }
        (*ioOff)++;
        result |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
        if (shift >= 64) {
            return false; /* malformed: too many continuation bytes */
        }
    }
    *out = result;
    return true;
}

/* Expands one encoded name (a mix of literal ASCII bytes and token bytes >=0x80) into `out`,
 * truncating (like ksnprintf) rather than overflowing if it doesn't fit -- that's a caller-buffer
 * issue, not blob corruption. Returns false only for an actually malformed token reference
 * (out-of-range token index, or a token/pool region that doesn't fit in the blob). */
static bool expandName(const uint8_t *blob, size_t size, uint32_t tokenOffset, uint32_t tokenCount,
                       size_t nameOff, uint8_t nameLen, char *out, size_t outCap) {
    size_t pos = 0;
    size_t poolStart = (size_t)tokenOffset + (size_t)tokenCount * 4;
    for (uint8_t i = 0; i < nameLen; i++) {
        uint8_t b;
        if (!readU8(blob, size, nameOff + i, &b)) {
            return false;
        }
        if (b < 0x80) {
            if (pos + 1 < outCap) {
                out[pos++] = (char)b;
            }
            continue;
        }
        uint32_t tokenIdx = (uint32_t)(b - 0x80);
        if (tokenIdx >= tokenCount) {
            return false;
        }
        size_t entryOff = (size_t)tokenOffset + (size_t)tokenIdx * 4;
        uint16_t poolOff;
        uint8_t tokLen;
        if (!readU16(blob, size, entryOff, &poolOff) ||
            !readU8(blob, size, entryOff + 2, &tokLen)) {
            return false;
        }
        size_t expOff = poolStart + poolOff;
        for (uint8_t j = 0; j < tokLen; j++) {
            uint8_t eb;
            if (!readU8(blob, size, expOff + j, &eb)) {
                return false;
            }
            if (pos + 1 < outCap) {
                out[pos++] = (char)eb;
            }
        }
    }
    if (outCap > 0) {
        out[pos] = '\0';
    }
    return true;
}

Status ksymDecodeLookup(const uint8_t *blob, size_t size, uint64_t addr, char *nameOut,
                        size_t nameCap, uint64_t *symAddrOut) {
    if (blob == NULL || size < KSYM_HEADER_SIZE) {
        return STATUS_ERR_NOT_FOUND;
    }
    uint32_t magic;
    uint16_t version, headerSize;
    if (!readU32(blob, size, 0, &magic) || magic != KSYM_MAGIC ||
        !readU16(blob, size, 4, &version) || version != KSYM_VERSION ||
        !readU16(blob, size, 6, &headerSize) || headerSize < KSYM_HEADER_SIZE) {
        return STATUS_ERR_NOT_FOUND;
    }
    uint32_t count, blockShift, indexOffset, indexCount, tokenOffset, tokenCount, streamOffset,
        streamSize;
    uint64_t textBase, textEnd;
    if (!readU32(blob, size, 8, &count) || !readU32(blob, size, 12, &blockShift) ||
        !readU64(blob, size, 16, &textBase) || !readU64(blob, size, 24, &textEnd) ||
        !readU32(blob, size, 32, &indexOffset) || !readU32(blob, size, 36, &indexCount) ||
        !readU32(blob, size, 40, &tokenOffset) || !readU32(blob, size, 44, &tokenCount) ||
        !readU32(blob, size, 48, &streamOffset) || !readU32(blob, size, 52, &streamSize)) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (count == 0 || indexCount == 0 || blockShift > 31 || addr < textBase || addr >= textEnd) {
        return STATUS_ERR_NOT_FOUND;
    }
    if ((uint64_t)indexOffset + (uint64_t)indexCount * 16 > size ||
        (uint64_t)streamOffset + (uint64_t)streamSize > size) {
        return STATUS_ERR_NOT_FOUND;
    }
    uint32_t blockSize = 1u << blockShift;

    /* Binary search the block index for the rightmost block whose firstAddr <= addr (entries are
     * sorted ascending by address, guaranteed by the encoder). */
    uint32_t lo = 0, hi = indexCount, blockIdx = 0;
    bool haveBlock = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t firstAddr;
        if (!readU64(blob, size, (size_t)indexOffset + (size_t)mid * 16, &firstAddr)) {
            return STATUS_ERR_NOT_FOUND;
        }
        if (firstAddr <= addr) {
            blockIdx = mid;
            haveBlock = true;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (!haveBlock) {
        return STATUS_ERR_NOT_FOUND;
    }

    uint64_t blockFirstAddr;
    uint32_t blockStreamOff;
    if (!readU64(blob, size, (size_t)indexOffset + (size_t)blockIdx * 16, &blockFirstAddr) ||
        !readU32(blob, size, (size_t)indexOffset + (size_t)blockIdx * 16 + 8, &blockStreamOff)) {
        return STATUS_ERR_NOT_FOUND;
    }

    uint32_t startSymIndex = blockIdx * blockSize;
    uint32_t symsInBlock = blockSize;
    if (count - startSymIndex < symsInBlock) {
        symsInBlock = count - startSymIndex;
    }

    size_t off = (size_t)streamOffset + blockStreamOff;
    uint64_t curAddr = 0;
    uint64_t bestAddr = 0;
    size_t bestNameOff = 0;
    uint8_t bestNameLen = 0;
    bool haveBest = false;
    for (uint32_t i = 0; i < symsInBlock; i++) {
        uint64_t delta;
        if (!readUleb128(blob, size, &off, &delta)) {
            return STATUS_ERR_NOT_FOUND;
        }
        curAddr = (i == 0) ? blockFirstAddr : curAddr + delta;

        uint8_t encLen;
        if (!readU8(blob, size, off, &encLen)) {
            return STATUS_ERR_NOT_FOUND;
        }
        off += 1;
        if (off + encLen > size) {
            return STATUS_ERR_NOT_FOUND;
        }
        if (curAddr > addr) {
            break; /* addresses only increase within a block, so nothing further can match */
        }
        bestAddr = curAddr;
        bestNameOff = off;
        bestNameLen = encLen;
        haveBest = true;
        off += encLen;
    }
    if (!haveBest) {
        return STATUS_ERR_NOT_FOUND;
    }

    if (!expandName(blob, size, tokenOffset, tokenCount, bestNameOff, bestNameLen, nameOut,
                    nameCap)) {
        return STATUS_ERR_NOT_FOUND;
    }
    *symAddrOut = bestAddr;
    return STATUS_OK;
}
