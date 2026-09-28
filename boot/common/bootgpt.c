/* See bootgpt.h. Field offsets/sizes are the UEFI Spec §5.3.2/§5.3.3 GPT header and partition
 * entry layouts. Every multi-byte field is read with bootMemcpy at a fixed offset rather than
 * pointer-cast onto the sector buffer (same reasoning as boot/common/elf.c: the buffer's
 * alignment isn't guaranteed, and this keeps UBSan quiet in host tests). */
#include "include/bootgpt.h"

#include "include/bootcrc32.h"
#include "include/bootmem.h"

#include <stdbool.h>

const uint8_t BOOT_GPT_TYPE_GUID_ESP[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                            0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};

#define GPT_HEADER_SIG_OFFSET     0u
#define GPT_HEADER_SIG_LEN        8u
#define GPT_HEADER_REVISION_OFF   8u
#define GPT_HEADER_SIZE_OFF       12u
#define GPT_HEADER_CRC32_OFF      16u
#define GPT_HEADER_MYLBA_OFF      24u
#define GPT_HEADER_FIRSTUSABLE_OFF 40u
#define GPT_HEADER_LASTUSABLE_OFF 48u
#define GPT_HEADER_DISKGUID_OFF   56u
#define GPT_HEADER_ENTRYLBA_OFF   72u
#define GPT_HEADER_ENTRYCOUNT_OFF 80u
#define GPT_HEADER_ENTRYSIZE_OFF  84u
#define GPT_HEADER_ARRAYCRC32_OFF 88u
#define GPT_HEADER_MIN_SIZE       92u
#define GPT_MAX_PARTITION_ENTRIES 1024u
#define GPT_MIN_ENTRY_SIZE        128u

#define GPT_ENTRY_TYPEGUID_OFF   0u
#define GPT_ENTRY_UNIQUEGUID_OFF 16u
#define GPT_ENTRY_STARTLBA_OFF   32u
#define GPT_ENTRY_ENDLBA_OFF     40u

typedef struct {
    uint64_t myLba;
    uint64_t firstUsableLba, lastUsableLba;
    uint8_t diskGuid[16];
    uint64_t entryLba;
    uint32_t entryCount;
    uint32_t entrySize;
    uint32_t arrayCrc32;
} GptHeaderFields;

static bool bytesEqual(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static bool isPowerOfTwo(uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* Reads and validates the header at `lba` (expected to equal the header's own MyLBA). On success,
 * fills `*fields` and returns true. Leaves `scratch` holding the just-read sector either way. */
static bool validateHeader(const BootBlockDev *dev, uint64_t lba, uint8_t *scratch,
                           GptHeaderFields *fields) {
    static const uint8_t sig[GPT_HEADER_SIG_LEN] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    if (dev->read(dev->ctx, lba, 1, scratch) != BOOT_OK) {
        return false;
    }
    if (!bytesEqual(scratch + GPT_HEADER_SIG_OFFSET, sig, GPT_HEADER_SIG_LEN)) {
        return false;
    }
    uint32_t revision, headerSize, storedCrc;
    bootMemcpy(&revision, scratch + GPT_HEADER_REVISION_OFF, sizeof(revision));
    bootMemcpy(&headerSize, scratch + GPT_HEADER_SIZE_OFF, sizeof(headerSize));
    bootMemcpy(&storedCrc, scratch + GPT_HEADER_CRC32_OFF, sizeof(storedCrc));
    if ((revision >> 16) != 1u) {
        return false;
    }
    if (headerSize < GPT_HEADER_MIN_SIZE || headerSize > dev->sectorSize) {
        return false;
    }

    uint32_t zero = 0;
    bootMemcpy(scratch + GPT_HEADER_CRC32_OFF, &zero, sizeof(zero));
    uint32_t computedCrc = bootCrc32(scratch, headerSize);
    bootMemcpy(scratch + GPT_HEADER_CRC32_OFF, &storedCrc, sizeof(storedCrc));
    if (computedCrc != storedCrc) {
        return false;
    }

    bootMemcpy(&fields->myLba, scratch + GPT_HEADER_MYLBA_OFF, sizeof(fields->myLba));
    if (fields->myLba != lba) {
        return false;
    }
    bootMemcpy(&fields->firstUsableLba, scratch + GPT_HEADER_FIRSTUSABLE_OFF,
              sizeof(fields->firstUsableLba));
    bootMemcpy(&fields->lastUsableLba, scratch + GPT_HEADER_LASTUSABLE_OFF,
              sizeof(fields->lastUsableLba));
    if (fields->firstUsableLba > fields->lastUsableLba) {
        return false;
    }
    bootMemcpy(fields->diskGuid, scratch + GPT_HEADER_DISKGUID_OFF, sizeof(fields->diskGuid));
    bootMemcpy(&fields->entryLba, scratch + GPT_HEADER_ENTRYLBA_OFF, sizeof(fields->entryLba));
    bootMemcpy(&fields->entryCount, scratch + GPT_HEADER_ENTRYCOUNT_OFF,
              sizeof(fields->entryCount));
    bootMemcpy(&fields->entrySize, scratch + GPT_HEADER_ENTRYSIZE_OFF, sizeof(fields->entrySize));
    bootMemcpy(&fields->arrayCrc32, scratch + GPT_HEADER_ARRAYCRC32_OFF,
              sizeof(fields->arrayCrc32));
    if (fields->entryCount > GPT_MAX_PARTITION_ENTRIES) {
        return false;
    }
    if (fields->entrySize < GPT_MIN_ENTRY_SIZE || fields->entrySize > dev->sectorSize ||
        !isPowerOfTwo(fields->entrySize) || dev->sectorSize % fields->entrySize != 0) {
        return false;
    }
    return true;
}

/* Streams the partition-entry array (one sector at a time through `scratch`): verifies its
 * CRC-32 against `fields->arrayCrc32`, and, while doing so, looks for the first entry matching
 * `typeGuid` inside the header's usable range. Returns BOOT_OK with `*out` filled if found,
 * BOOT_ERR_NOT_FOUND if the array validates but nothing matches, BOOT_ERR_GPT if the array CRC
 * doesn't match, or BOOT_ERR_IO on a read failure. */
static BootStatus scanArray(const BootBlockDev *dev, const GptHeaderFields *fields,
                            const uint8_t typeGuid[16], uint8_t *scratch, BootGptPart *out) {
    uint64_t totalBytes = (uint64_t)fields->entryCount * fields->entrySize;
    uint64_t bytesLeft = totalBytes;
    uint32_t crc = BOOT_CRC32_INIT;
    bool found = false;
    BootGptPart match = {0};

    uint32_t entryIndex = 0;
    for (uint64_t sectorOffset = 0; bytesLeft > 0; sectorOffset++) {
        if (dev->read(dev->ctx, fields->entryLba + sectorOffset, 1, scratch) != BOOT_OK) {
            return BOOT_ERR_IO;
        }
        uint32_t chunk = dev->sectorSize;
        if ((uint64_t)chunk > bytesLeft) {
            chunk = (uint32_t)bytesLeft;
        }
        crc = bootCrc32Update(crc, scratch, chunk);
        bytesLeft -= chunk;

        uint32_t entriesInChunk = chunk / fields->entrySize;
        for (uint32_t i = 0; i < entriesInChunk && entryIndex < fields->entryCount;
            i++, entryIndex++) {
            const uint8_t *e = scratch + (uint64_t)i * fields->entrySize;
            if (found) {
                continue; /* still need to finish the CRC over the whole array */
            }
            if (!bytesEqual(e + GPT_ENTRY_TYPEGUID_OFF, typeGuid, 16)) {
                continue;
            }
            uint64_t startLba, endLba;
            bootMemcpy(&startLba, e + GPT_ENTRY_STARTLBA_OFF, sizeof(startLba));
            bootMemcpy(&endLba, e + GPT_ENTRY_ENDLBA_OFF, sizeof(endLba));
            if (startLba > endLba || startLba < fields->firstUsableLba ||
                endLba > fields->lastUsableLba) {
                continue;
            }
            bootMemcpy(match.typeGuid, e + GPT_ENTRY_TYPEGUID_OFF, sizeof(match.typeGuid));
            bootMemcpy(match.uniqueGuid, e + GPT_ENTRY_UNIQUEGUID_OFF, sizeof(match.uniqueGuid));
            match.startLba = startLba;
            match.endLba = endLba;
            found = true;
        }
    }

    if (bootCrc32Finish(crc) != fields->arrayCrc32) {
        return BOOT_ERR_GPT;
    }
    if (!found) {
        return BOOT_ERR_NOT_FOUND;
    }
    *out = match;
    return BOOT_OK;
}

BootStatus bootGptFindPartition(const BootBlockDev *dev, const uint8_t typeGuid[16],
                                uint8_t *scratch, BootGptPart *out, uint8_t diskGuid[16]) {
    if (dev->sectorSize == 0) {
        return BOOT_ERR_GPT;
    }

    /* "Fails validation" covers the whole primary header+array, not just the header's own
     * fields -- a bad array CRC (or a read failure partway through it) falls back to the backup
     * exactly like a structurally invalid header would. A validated header with no matching
     * entry (BOOT_ERR_NOT_FOUND) never falls back: the backup's table would have the same
     * partitions. */
    GptHeaderFields fields;
    BootStatus st = BOOT_ERR_GPT;
    if (validateHeader(dev, 1, scratch, &fields)) {
        st = scanArray(dev, &fields, typeGuid, scratch, out);
    }
    if (st != BOOT_OK && st != BOOT_ERR_NOT_FOUND && dev->sectorCount != 0) {
        st = validateHeader(dev, dev->sectorCount - 1, scratch, &fields)
                ? scanArray(dev, &fields, typeGuid, scratch, out)
                : BOOT_ERR_GPT;
    }
    if (st == BOOT_OK) {
        bootMemcpy(diskGuid, fields.diskGuid, 16);
    }
    return st;
}
