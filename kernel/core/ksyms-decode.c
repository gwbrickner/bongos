/* See ksyms-format.h. */
#include "ksyms-format.h"

static int readU8(const uint8_t *blob, uint64_t len, uint64_t at, uint8_t *out) {
    if (at + 1 > len) {
        return 0;
    }
    *out = blob[at];
    return 1;
}

static int readU16(const uint8_t *blob, uint64_t len, uint64_t at, uint16_t *out) {
    if (at + 2 > len) {
        return 0;
    }
    *out = (uint16_t)((uint16_t)blob[at] | ((uint16_t)blob[at + 1] << 8));
    return 1;
}

static int readU32(const uint8_t *blob, uint64_t len, uint64_t at, uint32_t *out) {
    if (at + 4 > len) {
        return 0;
    }
    *out = (uint32_t)blob[at] | ((uint32_t)blob[at + 1] << 8) | ((uint32_t)blob[at + 2] << 16) |
           ((uint32_t)blob[at + 3] << 24);
    return 1;
}

Status ksymsLookup(const uint8_t *blob, uint64_t len, uint64_t off, KsymsSymbol *out) {
    uint32_t magic, count, restartInterval, addrsOff, restartsOff, namesOff, namesSize;
    uint16_t version, headerSize;

    if (!readU32(blob, len, KSYMS_OFF_MAGIC, &magic) || magic != KSYMS_MAGIC) {
        return STATUS_ERR_INVALID;
    }
    if (!readU16(blob, len, KSYMS_OFF_VERSION, &version) || version != KSYMS_VERSION) {
        return STATUS_ERR_INVALID;
    }
    if (!readU16(blob, len, KSYMS_OFF_HEADER_SIZE, &headerSize) ||
        headerSize != KSYMS_HEADER_SIZE) {
        return STATUS_ERR_INVALID;
    }
    if (!readU32(blob, len, KSYMS_OFF_COUNT, &count) || count > KSYMS_MAX_COUNT) {
        return STATUS_ERR_INVALID;
    }
    if (!readU32(blob, len, KSYMS_OFF_RESTART_INTERVAL, &restartInterval)) {
        return STATUS_ERR_INVALID;
    }
    if (restartInterval == 0 || restartInterval > KSYMS_MAX_RESTART ||
        (restartInterval & (restartInterval - 1)) != 0) {
        return STATUS_ERR_INVALID;
    }
    if (!readU32(blob, len, KSYMS_OFF_ADDRS_OFF, &addrsOff) || addrsOff != KSYMS_HEADER_SIZE) {
        return STATUS_ERR_INVALID;
    }
    if (!readU32(blob, len, KSYMS_OFF_RESTARTS_OFF, &restartsOff)) {
        return STATUS_ERR_INVALID;
    }
    if (!readU32(blob, len, KSYMS_OFF_NAMES_OFF, &namesOff)) {
        return STATUS_ERR_INVALID;
    }
    if (!readU32(blob, len, KSYMS_OFF_NAMES_SIZE, &namesSize)) {
        return STATUS_ERR_INVALID;
    }

    uint64_t restartCount =
        count == 0 ? 0 : (((uint64_t)count + restartInterval - 1) / restartInterval);
    uint64_t expectedRestartsOff = (uint64_t)addrsOff + 8ull * count;
    uint64_t expectedNamesOff = expectedRestartsOff + 4ull * restartCount;
    if (restartsOff != expectedRestartsOff || namesOff != expectedNamesOff) {
        return STATUS_ERR_INVALID;
    }
    if ((uint64_t)addrsOff + 8ull * count > len ||
        (uint64_t)restartsOff + 4ull * restartCount > len || (uint64_t)namesOff + namesSize > len) {
        return STATUS_ERR_INVALID;
    }
    if (count == 0) {
        return STATUS_ERR_NOT_FOUND;
    }

    /* Binary search for the last entry whose offset is <= off. */
    uint64_t lo = 0, hi = count;
    while (lo + 1 < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint32_t midOff;
        if (!readU32(blob, len, addrsOff + mid * 8, &midOff)) {
            return STATUS_ERR_INVALID;
        }
        if ((uint64_t)midOff <= off) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    uint32_t entryOffset, entrySize;
    if (!readU32(blob, len, addrsOff + lo * 8, &entryOffset) ||
        !readU32(blob, len, addrsOff + lo * 8 + 4, &entrySize)) {
        return STATUS_ERR_INVALID;
    }
    if (off < entryOffset || off - entryOffset >= entrySize) {
        return STATUS_ERR_NOT_FOUND;
    }

    /* Decode the name by front-coding from the nearest restart point. */
    uint64_t restartIdx = lo / restartInterval;
    uint32_t restartByteOff;
    if (!readU32(blob, len, restartsOff + restartIdx * 4, &restartByteOff) ||
        restartByteOff > namesSize) {
        return STATUS_ERR_INVALID;
    }

    char name[KSYMS_NAME_MAX];
    uint32_t nameLen = 0;
    uint64_t cursor = (uint64_t)namesOff + restartByteOff;
    uint64_t steps = lo - restartIdx * restartInterval;
    for (uint64_t s = 0; s <= steps; s++) {
        uint8_t shared, suffixLen;
        if (!readU8(blob, len, cursor, &shared) || !readU8(blob, len, cursor + 1, &suffixLen)) {
            return STATUS_ERR_INVALID;
        }
        if (shared > nameLen || (uint32_t)shared + suffixLen > KSYMS_NAME_MAX - 1) {
            return STATUS_ERR_INVALID;
        }
        if (cursor + 2 + suffixLen > (uint64_t)namesOff + namesSize) {
            return STATUS_ERR_INVALID;
        }
        for (uint32_t k = 0; k < suffixLen; k++) {
            uint8_t b;
            if (!readU8(blob, len, cursor + 2 + k, &b)) {
                return STATUS_ERR_INVALID;
            }
            name[(uint32_t)shared + k] = (char)b;
        }
        nameLen = (uint32_t)shared + suffixLen;
        cursor += 2 + suffixLen;
    }
    name[nameLen] = '\0';

    out->offset = entryOffset;
    out->size = entrySize;
    for (uint32_t k = 0; k <= nameLen; k++) {
        out->name[k] = name[k];
    }
    return STATUS_OK;
}
