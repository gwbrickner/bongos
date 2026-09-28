/* See bootfat.h. Field offsets are the Microsoft FAT spec v1.03's BPB/FAT32 boot sector and
 * 32-byte directory-entry layouts. Every multi-byte field is read with bootMemcpy at a fixed
 * offset rather than pointer-cast (same reasoning as bootgpt.c/elf.c). */
#include "include/bootfat.h"

#include "include/bootmem.h"

#include <stdbool.h>

#define BPB_JMPBOOT_OFF     0u
#define BPB_BYTSPERSEC_OFF  11u
#define BPB_SECPERCLUS_OFF  13u
#define BPB_RSVDSECCNT_OFF  14u
#define BPB_NUMFATS_OFF     16u
#define BPB_ROOTENTCNT_OFF  17u
#define BPB_TOTSEC16_OFF    19u
#define BPB_FATSZ16_OFF     22u
#define BPB_TOTSEC32_OFF    32u
#define BPB_FATSZ32_OFF     36u
#define BPB_EXTFLAGS_OFF    40u
#define BPB_FSVER_OFF       42u
#define BPB_ROOTCLUS_OFF    44u
#define BPB_BOOTSIG_OFF     510u

#define FAT_EOC_MIN      0x0FFFFFF8u
#define FAT_BAD_CLUSTER  0x0FFFFFF7u
#define FAT_ENTRY_MASK   0x0FFFFFFFu

#define DIR_ENTRY_SIZE     32u
#define DIR_NAME_OFF       0u
#define DIR_ATTR_OFF       11u
#define DIR_FSTCLUSHI_OFF  20u
#define DIR_FSTCLUSLO_OFF  26u
#define DIR_FILESIZE_OFF   28u

#define ATTR_READ_ONLY 0x01u
#define ATTR_HIDDEN    0x02u
#define ATTR_SYSTEM    0x04u
#define ATTR_VOLUME_ID 0x08u
#define ATTR_DIRECTORY 0x10u
#define ATTR_LFN       0x0Fu /* READ_ONLY|HIDDEN|SYSTEM|VOLUME_ID all set */

#define LFN_LAST_ENTRY_FLAG 0x40u
#define LFN_SEQ_MASK        0x1Fu
#define LFN_CHARS_PER_ENTRY 13u
#define LFN_MAX_SEQ         20u /* 20*13 = 260 >= the 255-byte component-length cap */
#define LFN_ORD_OFF         0u
#define LFN_NAME1_OFF       1u  /* 5 UTF-16 code units */
#define LFN_CHKSUM_OFF      13u
#define LFN_NAME2_OFF       14u /* 6 UTF-16 code units */
#define LFN_NAME3_OFF       28u /* 2 UTF-16 code units */

#define BOOT_FAT_PATH_MAX_COMPONENTS 16u
#define BOOT_FAT_COMPONENT_MAX       255u
#define BOOT_FAT_LFN_BUF_LEN         (LFN_MAX_SEQ * LFN_CHARS_PER_ENTRY)

static bool isPowerOfTwo(uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

BootStatus bootFatMount(BootFatVol *vol, const BootBlockDev *dev, uint64_t partLba,
                        uint64_t partSectors, uint8_t *scratch) {
    if (dev->read(dev->ctx, partLba, 1, scratch) != BOOT_OK) {
        return BOOT_ERR_IO;
    }
    uint8_t jmp0 = scratch[BPB_JMPBOOT_OFF];
    if (jmp0 != 0xEBu && jmp0 != 0xE9u) {
        return BOOT_ERR_FAT;
    }
    uint16_t bytsPerSec, rsvdSecCnt, rootEntCnt, totSec16, fatSz16, fsVer;
    uint8_t secPerClus, numFats;
    uint32_t totSec32, fatSz32, rootClus;
    bootMemcpy(&bytsPerSec, scratch + BPB_BYTSPERSEC_OFF, sizeof(bytsPerSec));
    secPerClus = scratch[BPB_SECPERCLUS_OFF];
    bootMemcpy(&rsvdSecCnt, scratch + BPB_RSVDSECCNT_OFF, sizeof(rsvdSecCnt));
    numFats = scratch[BPB_NUMFATS_OFF];
    bootMemcpy(&rootEntCnt, scratch + BPB_ROOTENTCNT_OFF, sizeof(rootEntCnt));
    bootMemcpy(&totSec16, scratch + BPB_TOTSEC16_OFF, sizeof(totSec16));
    bootMemcpy(&fatSz16, scratch + BPB_FATSZ16_OFF, sizeof(fatSz16));
    bootMemcpy(&totSec32, scratch + BPB_TOTSEC32_OFF, sizeof(totSec32));
    bootMemcpy(&fatSz32, scratch + BPB_FATSZ32_OFF, sizeof(fatSz32));
    bootMemcpy(&fsVer, scratch + BPB_FSVER_OFF, sizeof(fsVer));
    bootMemcpy(&rootClus, scratch + BPB_ROOTCLUS_OFF, sizeof(rootClus));
    uint16_t extFlags;
    bootMemcpy(&extFlags, scratch + BPB_EXTFLAGS_OFF, sizeof(extFlags));

    if (bytsPerSec != dev->sectorSize) {
        return BOOT_ERR_FAT;
    }
    if (!isPowerOfTwo(secPerClus) || secPerClus > 128) {
        return BOOT_ERR_FAT;
    }
    if (rsvdSecCnt < 1 || numFats < 1) {
        return BOOT_ERR_FAT;
    }
    if (rootEntCnt != 0 || totSec16 != 0 || fatSz16 != 0 || fatSz32 == 0 || fsVer != 0) {
        return BOOT_ERR_FAT; /* not FAT32-shaped */
    }
    if (totSec32 == 0 || (uint64_t)totSec32 > partSectors) {
        return BOOT_ERR_FAT;
    }
    if (scratch[BPB_BOOTSIG_OFF] != 0x55u || scratch[BPB_BOOTSIG_OFF + 1] != 0xAAu) {
        return BOOT_ERR_FAT;
    }

    uint64_t dataSectors = (uint64_t)totSec32 - (rsvdSecCnt + (uint64_t)numFats * fatSz32);
    uint32_t countOfClusters = (uint32_t)(dataSectors / secPerClus);
    if (countOfClusters < 65525u || countOfClusters >= 0x0FFFFFF5u) {
        return BOOT_ERR_FAT;
    }
    if (rootClus < 2 || rootClus > countOfClusters + 1) {
        return BOOT_ERR_FAT;
    }

    uint32_t activeFat = 0;
    if (extFlags & 0x80u) {
        activeFat = extFlags & 0x0Fu;
        if (activeFat >= numFats) {
            return BOOT_ERR_FAT;
        }
    }

    vol->dev = dev;
    vol->bytesPerSec = bytsPerSec;
    vol->secPerClus = secPerClus;
    vol->fatSz32 = fatSz32;
    vol->rootClus = rootClus;
    vol->countOfClusters = countOfClusters;
    vol->fatLba = partLba + rsvdSecCnt + (uint64_t)activeFat * fatSz32;
    vol->dataLba = partLba + rsvdSecCnt + (uint64_t)numFats * fatSz32;
    return BOOT_OK;
}

/* Reads FAT[cluster] (masked to 28 bits). `scratch` must be >= vol->bytesPerSec bytes. */
static BootStatus readFatEntry(const BootFatVol *vol, uint32_t cluster, uint8_t *scratch,
                               uint32_t *outNext) {
    uint64_t byteOff = (uint64_t)cluster * 4;
    uint64_t sec = byteOff / vol->bytesPerSec;
    uint32_t off = (uint32_t)(byteOff % vol->bytesPerSec);
    if (sec >= vol->fatSz32) {
        return BOOT_ERR_FAT;
    }
    if (vol->dev->read(vol->dev->ctx, vol->fatLba + sec, 1, scratch) != BOOT_OK) {
        return BOOT_ERR_IO;
    }
    uint32_t raw;
    bootMemcpy(&raw, scratch + off, sizeof(raw));
    *outNext = raw & FAT_ENTRY_MASK;
    return BOOT_OK;
}

static uint64_t clusterFirstSector(const BootFatVol *vol, uint32_t cluster) {
    return vol->dataLba + (uint64_t)(cluster - 2) * vol->secPerClus;
}

/* case-insensitive ASCII compare of two byte strings with explicit lengths. */
static bool asciiEqualsIgnoreCase(const char *a, uint32_t aLen, const char *b, uint32_t bLen) {
    if (aLen != bLen) {
        return false;
    }
    for (uint32_t i = 0; i < aLen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') {
            ca = (char)(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = (char)(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/* Renders an 11-byte 8.3 name field as "BASE" or "BASE.EXT" into `out` (>= 13 bytes). Returns the
 * rendered length. */
static uint32_t renderShortName(const uint8_t nameField[11], char out[13]) {
    uint8_t base0 = nameField[0] == 0x05u ? 0xE5u : nameField[0]; /* the 0xE5 escape */
    uint32_t baseLen = 8;
    while (baseLen > 0 && nameField[baseLen - 1] == ' ') {
        baseLen--;
    }
    uint32_t extLen = 3;
    while (extLen > 0 && nameField[8 + extLen - 1] == ' ') {
        extLen--;
    }
    uint32_t pos = 0;
    for (uint32_t i = 0; i < baseLen; i++) {
        out[pos++] = (char)(i == 0 ? (char)base0 : (char)nameField[i]);
    }
    if (extLen > 0) {
        out[pos++] = '.';
        for (uint32_t i = 0; i < extLen; i++) {
            out[pos++] = (char)nameField[8 + i];
        }
    }
    return pos;
}

/* Standard VFAT short-name checksum (Microsoft FAT spec). */
static uint8_t shortNameChecksum(const uint8_t nameField[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + nameField[i]);
    }
    return sum;
}

typedef struct {
    char buf[BOOT_FAT_LFN_BUF_LEN];
    uint32_t len;      /* valid only when hasTerm is true */
    bool hasTerm;
    bool nonAscii;
    bool active;       /* an in-progress or completed group awaits its short entry */
    uint32_t expectSeq; /* next (lower) sequence number expected */
    uint8_t checksum;
} LfnAccum;

static void lfnReset(LfnAccum *a) {
    a->active = false;
    a->hasTerm = false;
    a->nonAscii = false;
    a->len = 0;
}

static void lfnFeed(LfnAccum *a, const uint8_t *entry) {
    uint8_t ord = entry[LFN_ORD_OFF];
    uint32_t seq = ord & LFN_SEQ_MASK;
    bool isLast = (ord & LFN_LAST_ENTRY_FLAG) != 0;
    if (seq < 1 || seq > LFN_MAX_SEQ) {
        lfnReset(a);
        return;
    }
    if (isLast) {
        lfnReset(a);
        a->active = true;
        a->expectSeq = seq;
        a->checksum = entry[LFN_CHKSUM_OFF];
    } else if (!a->active || seq != a->expectSeq - 1 || entry[LFN_CHKSUM_OFF] != a->checksum) {
        lfnReset(a);
        return;
    }
    a->expectSeq = seq;

    uint16_t units[LFN_CHARS_PER_ENTRY];
    bootMemcpy(&units[0], entry + LFN_NAME1_OFF, 5 * sizeof(uint16_t));
    bootMemcpy(&units[5], entry + LFN_NAME2_OFF, 6 * sizeof(uint16_t));
    bootMemcpy(&units[11], entry + LFN_NAME3_OFF, 2 * sizeof(uint16_t));

    uint32_t base = (seq - 1) * LFN_CHARS_PER_ENTRY;
    for (uint32_t i = 0; i < LFN_CHARS_PER_ENTRY; i++) {
        uint16_t u = units[i];
        if (u == 0) {
            a->hasTerm = true;
            a->len = base + i;
            break;
        }
        if (u > 0x7Fu) {
            a->nonAscii = true;
        }
        if (base + i < BOOT_FAT_LFN_BUF_LEN) {
            a->buf[base + i] = (char)(u & 0x7Fu);
        }
    }
}

/* Scans one directory's cluster chain for `component` (case-insensitive ASCII, matching a
 * checksum-verified LFN or the rendered 8.3 short name). Fills `*outAttr`/`*outCluster`/
 * `*outSize` on success. */
static BootStatus dirFindEntry(const BootFatVol *vol, uint32_t dirCluster, const char *component,
                               uint32_t componentLen, uint8_t *scratch, uint32_t *outAttr,
                               uint32_t *outCluster, uint32_t *outSize) {
    LfnAccum lfn;
    lfnReset(&lfn);
    uint32_t cluster = dirCluster;
    uint32_t steps = 0;
    while (cluster >= 2 && cluster <= vol->countOfClusters + 1) {
        if (steps++ > vol->countOfClusters) {
            return BOOT_ERR_FAT;
        }
        uint64_t firstSec = clusterFirstSector(vol, cluster);
        for (uint32_t s = 0; s < vol->secPerClus; s++) {
            if (vol->dev->read(vol->dev->ctx, firstSec + s, 1, scratch) != BOOT_OK) {
                return BOOT_ERR_IO;
            }
            uint32_t entriesPerSector = vol->bytesPerSec / DIR_ENTRY_SIZE;
            for (uint32_t e = 0; e < entriesPerSector; e++) {
                const uint8_t *entry = scratch + e * DIR_ENTRY_SIZE;
                uint8_t first = entry[DIR_NAME_OFF];
                if (first == 0x00u) {
                    return BOOT_ERR_NOT_FOUND; /* end of directory */
                }
                if (first == 0xE5u) {
                    continue; /* deleted */
                }
                uint8_t attr = entry[DIR_ATTR_OFF];
                if (attr == ATTR_LFN) {
                    lfnFeed(&lfn, entry);
                    continue;
                }
                if (attr & ATTR_VOLUME_ID) {
                    lfnReset(&lfn);
                    continue;
                }
                bool matched = false;
                if (lfn.active && lfn.hasTerm && !lfn.nonAscii &&
                    shortNameChecksum(entry + DIR_NAME_OFF) == lfn.checksum) {
                    matched = asciiEqualsIgnoreCase(lfn.buf, lfn.len, component, componentLen);
                }
                if (!matched) {
                    char rendered[13];
                    uint32_t renderedLen = renderShortName(entry + DIR_NAME_OFF, rendered);
                    matched = asciiEqualsIgnoreCase(rendered, renderedLen, component, componentLen);
                }
                lfnReset(&lfn);
                if (matched) {
                    uint16_t hi, lo;
                    uint32_t size;
                    bootMemcpy(&hi, entry + DIR_FSTCLUSHI_OFF, sizeof(hi));
                    bootMemcpy(&lo, entry + DIR_FSTCLUSLO_OFF, sizeof(lo));
                    bootMemcpy(&size, entry + DIR_FILESIZE_OFF, sizeof(size));
                    *outAttr = attr;
                    *outCluster = ((uint32_t)hi << 16) | lo;
                    *outSize = size;
                    return BOOT_OK;
                }
            }
        }
        BootStatus st = readFatEntry(vol, cluster, scratch, &cluster);
        if (st != BOOT_OK) {
            return st;
        }
        if (cluster >= FAT_EOC_MIN) {
            return BOOT_ERR_NOT_FOUND;
        }
        if (cluster == FAT_BAD_CLUSTER) {
            return BOOT_ERR_FAT;
        }
    }
    return BOOT_ERR_FAT;
}

BootStatus bootFatOpen(const BootFatVol *vol, const char *path, uint8_t *scratch,
                       BootFatFile *out) {
    if (path == NULL || path[0] != '/') {
        return BOOT_ERR_NOT_FOUND;
    }
    uint32_t dirCluster = vol->rootClus;
    uint32_t pos = 1;
    uint32_t componentCount = 0;
    for (;;) {
        uint32_t start = pos;
        while (path[pos] != '\0' && path[pos] != '/') {
            pos++;
        }
        uint32_t len = pos - start;
        if (len == 0) {
            return BOOT_ERR_NOT_FOUND; /* empty component (e.g. a double slash) */
        }
        if (len > BOOT_FAT_COMPONENT_MAX) {
            return BOOT_ERR_TOO_LARGE;
        }
        if (++componentCount > BOOT_FAT_PATH_MAX_COMPONENTS) {
            return BOOT_ERR_NOT_FOUND;
        }
        bool isLast = path[pos] == '\0';

        uint32_t attr, cluster, size;
        BootStatus st =
            dirFindEntry(vol, dirCluster, path + start, len, scratch, &attr, &cluster, &size);
        if (st != BOOT_OK) {
            return st;
        }
        if (isLast) {
            if (attr & ATTR_DIRECTORY) {
                return BOOT_ERR_NOT_FOUND; /* a directory isn't a file to load */
            }
            out->firstCluster = cluster;
            out->size = size;
            return BOOT_OK;
        }
        if ((attr & ATTR_DIRECTORY) == 0) {
            return BOOT_ERR_FAT; /* an intermediate component must be a directory */
        }
        dirCluster = cluster;
        pos++; /* skip the '/' */
    }
}

BootStatus bootFatRead(const BootFatVol *vol, const BootFatFile *file, uint8_t *dst,
                       uint64_t dstCap, uint8_t *scratch) {
    if (file->size > dstCap) {
        return BOOT_ERR_TOO_LARGE;
    }
    if (file->size == 0) {
        return BOOT_OK;
    }
    uint64_t clusterBytes = (uint64_t)vol->secPerClus * vol->bytesPerSec;
    uint64_t remaining = file->size;
    uint64_t dstOff = 0;
    uint32_t runStart = file->firstCluster;
    uint32_t steps = 0;

    while (remaining > 0) {
        if (runStart < 2 || runStart > vol->countOfClusters + 1) {
            return BOOT_ERR_FAT; /* chain ended before the promised size was read */
        }

        /* Merge a contiguous run of clusters into one block-device read: grow `runLen` while the
         * chain keeps incrementing by 1 and there's still more of the file left to cover. */
        uint32_t runLen = 1;
        uint32_t last = runStart;
        while ((uint64_t)runLen * clusterBytes < remaining) {
            if (steps++ > vol->countOfClusters) {
                return BOOT_ERR_FAT;
            }
            uint32_t next;
            BootStatus st = readFatEntry(vol, last, scratch, &next);
            if (st != BOOT_OK) {
                return st;
            }
            if (next != last + 1) {
                break;
            }
            last = next;
            runLen++;
        }

        uint64_t runBytes = (uint64_t)runLen * clusterBytes;
        uint64_t thisChunk = runBytes < remaining ? runBytes : remaining;
        uint64_t firstSec = clusterFirstSector(vol, runStart);
        uint64_t fullSectors = thisChunk / vol->bytesPerSec;
        uint64_t tailBytes = thisChunk - fullSectors * vol->bytesPerSec;

        if (fullSectors > 0) {
            if (vol->dev->read(vol->dev->ctx, firstSec, (uint32_t)fullSectors, dst + dstOff) !=
                BOOT_OK) {
                return BOOT_ERR_IO;
            }
        }
        if (tailBytes > 0) {
            /* Only the final, possibly-partial sector goes through scratch, so a short last
             * read never writes past dst[0, file->size). */
            if (vol->dev->read(vol->dev->ctx, firstSec + fullSectors, 1, scratch) != BOOT_OK) {
                return BOOT_ERR_IO;
            }
            bootMemcpy(dst + dstOff + fullSectors * vol->bytesPerSec, scratch, tailBytes);
        }

        dstOff += thisChunk;
        remaining -= thisChunk;
        if (remaining == 0) {
            break;
        }

        if (steps++ > vol->countOfClusters) {
            return BOOT_ERR_FAT;
        }
        uint32_t next;
        BootStatus st = readFatEntry(vol, last, scratch, &next);
        if (st != BOOT_OK) {
            return st;
        }
        if (next >= FAT_EOC_MIN || next == FAT_BAD_CLUSTER) {
            return BOOT_ERR_FAT; /* chain ended before the promised size was fully read */
        }
        runStart = next;
    }
    return BOOT_OK;
}
