/* See ksyms-encode.h and docs/specs/ksyms.md. */
#include "ksyms-encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KSYM_MAGIC               0x4D59534Bu /* "KSYM", LE */
#define KSYM_VERSION             1u
#define KSYM_HEADER_SIZE         64u
#define KSYM_BLOCK_SHIFT         6u /* 1<<6 = 64 symbols per block index entry */
#define KSYM_BLOCK_SIZE          (1u << KSYM_BLOCK_SHIFT)
#define KSYM_MAX_TOKENS          128
#define KSYM_MAX_TOKEN_EXPANSION 32
#define KSYM_MIN_PAIR_COUNT      4

static _Noreturn void die(const char *msg) {
    fprintf(stderr, "ksyms: %s\n", msg);
    exit(1);
}

/* ---- a small growable byte buffer, used to assemble the blob left to right ---- */
typedef struct {
    uint8_t *data;
    size_t len, cap;
} ByteBuf;

static void bufInit(ByteBuf *b) {
    b->data = NULL;
    b->len = b->cap = 0;
}
static void bufReserve(ByteBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) {
        return;
    }
    size_t newCap = b->cap == 0 ? 256 : b->cap;
    while (newCap < b->len + extra) {
        newCap *= 2;
    }
    b->data = realloc(b->data, newCap);
    if (b->data == NULL) {
        die("out of memory");
    }
    b->cap = newCap;
}
static void bufU8(ByteBuf *b, uint8_t v) {
    bufReserve(b, 1);
    b->data[b->len++] = v;
}
static void bufU16(ByteBuf *b, uint16_t v) {
    bufU8(b, (uint8_t)(v & 0xFF));
    bufU8(b, (uint8_t)((v >> 8) & 0xFF));
}
static void bufU32(ByteBuf *b, uint32_t v) {
    bufU16(b, (uint16_t)(v & 0xFFFF));
    bufU16(b, (uint16_t)((v >> 16) & 0xFFFF));
}
static void bufU64(ByteBuf *b, uint64_t v) {
    bufU32(b, (uint32_t)(v & 0xFFFFFFFFu));
    bufU32(b, (uint32_t)(v >> 32));
}
static void bufBytes(ByteBuf *b, const uint8_t *bytes, size_t n) {
    if (n == 0) {
        return; /* memcpy's source must be non-NULL even for n==0 (UB otherwise); an empty
                 * section (e.g. no BPE tokens at all) legitimately passes a NULL/empty buffer */
    }
    bufReserve(b, n);
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
}
static void bufUleb128(ByteBuf *b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v != 0) {
            byte |= 0x80;
        }
        bufU8(b, byte);
    } while (v != 0);
}

/* ---- byte-pair-token compression (D-075) ---- */
typedef struct {
    uint8_t *bytes;
    size_t len;
} Seq;

typedef struct {
    uint8_t expansion[KSYM_MAX_TOKEN_EXPANSION];
    uint8_t len;
} Token;

static void bpeCompress(Seq *seqs, size_t seqCount, Token *tokens, int *outTokenCount) {
    int tokenCount = 0;
    uint32_t *counts = malloc(65536 * sizeof(uint32_t));
    if (counts == NULL) {
        die("out of memory");
    }

    for (int round = 0; round < KSYM_MAX_TOKENS && tokenCount < KSYM_MAX_TOKENS; round++) {
        memset(counts, 0, 65536 * sizeof(uint32_t));
        for (size_t s = 0; s < seqCount; s++) {
            for (size_t i = 0; i + 1 < seqs[s].len; i++) {
                uint32_t pair = ((uint32_t)seqs[s].bytes[i] << 8) | seqs[s].bytes[i + 1];
                counts[pair]++;
            }
        }

        int bestA = -1, bestB = -1;
        uint32_t bestCount = 0;
        uint8_t bestExp[KSYM_MAX_TOKEN_EXPANSION];
        uint8_t bestLen = 0;
        for (int pair = 0; pair < 65536; pair++) {
            if (counts[pair] < KSYM_MIN_PAIR_COUNT) {
                continue;
            }
            int a = pair >> 8, b = pair & 0xFF;
            uint8_t lenA = (a < 0x80) ? 1 : tokens[a - 0x80].len;
            uint8_t lenB = (b < 0x80) ? 1 : tokens[b - 0x80].len;
            if ((int)lenA + (int)lenB > KSYM_MAX_TOKEN_EXPANSION) {
                continue; /* skip a pair whose flattened expansion would exceed 32 bytes */
            }
            if (counts[pair] > bestCount) {
                bestCount = counts[pair];
                bestA = a;
                bestB = b;
                bestLen = (uint8_t)(lenA + lenB);
                if (a < 0x80) {
                    bestExp[0] = (uint8_t)a;
                } else {
                    memcpy(bestExp, tokens[a - 0x80].expansion, lenA);
                }
                if (b < 0x80) {
                    bestExp[lenA] = (uint8_t)b;
                } else {
                    memcpy(bestExp + lenA, tokens[b - 0x80].expansion, lenB);
                }
            }
        }
        if (bestCount == 0) {
            break; /* no pair repeats often enough (or fits) to be worth tokenizing */
        }

        memcpy(tokens[tokenCount].expansion, bestExp, bestLen);
        tokens[tokenCount].len = bestLen;
        uint8_t newToken = (uint8_t)(0x80 + tokenCount);
        tokenCount++;

        for (size_t s = 0; s < seqCount; s++) {
            size_t w = 0;
            for (size_t r = 0; r < seqs[s].len;) {
                if (r + 1 < seqs[s].len && seqs[s].bytes[r] == (uint8_t)bestA &&
                    seqs[s].bytes[r + 1] == (uint8_t)bestB) {
                    seqs[s].bytes[w++] = newToken;
                    r += 2;
                } else {
                    seqs[s].bytes[w++] = seqs[s].bytes[r++];
                }
            }
            seqs[s].len = w;
        }
    }

    free(counts);
    *outTokenCount = tokenCount;
}

KsymsBlob ksymsEncodeEmpty(void) {
    ByteBuf buf;
    bufInit(&buf);
    bufU32(&buf, KSYM_MAGIC);
    bufU16(&buf, KSYM_VERSION);
    bufU16(&buf, KSYM_HEADER_SIZE);
    bufU32(&buf, 0); /* count */
    bufU32(&buf, KSYM_BLOCK_SHIFT);
    bufU64(&buf, 0);                /* textBase */
    bufU64(&buf, 0);                /* textEnd */
    bufU32(&buf, KSYM_HEADER_SIZE); /* indexOffset */
    bufU32(&buf, 0);                /* indexCount */
    bufU32(&buf, KSYM_HEADER_SIZE); /* tokenOffset */
    bufU32(&buf, 0);                /* tokenCount */
    bufU32(&buf, KSYM_HEADER_SIZE); /* streamOffset */
    bufU32(&buf, 0);                /* streamSize */
    bufU64(&buf, 0);                /* reserved */
    KsymsBlob blob = {buf.data, buf.len};
    return blob;
}

KsymsBlob ksymsEncode(const ElfFuncSymList *syms, uint64_t textBase, uint64_t textEnd) {
    if (syms->count == 0) {
        return ksymsEncodeEmpty();
    }
    for (size_t i = 0; i < syms->count; i++) {
        if (syms->syms[i].addr < textBase || syms->syms[i].addr >= textEnd) {
            die("a symbol address falls outside [textBase, textEnd)");
        }
        if (i > 0 && syms->syms[i].addr <= syms->syms[i - 1].addr) {
            die("symbols are not strictly sorted by address");
        }
    }

    size_t n = syms->count;
    Seq *seqs = calloc(n, sizeof(*seqs));
    for (size_t i = 0; i < n; i++) {
        seqs[i].len = strlen(syms->syms[i].name);
        seqs[i].bytes = malloc(seqs[i].len > 0 ? seqs[i].len : 1);
        memcpy(seqs[i].bytes, syms->syms[i].name, seqs[i].len);
    }

    Token tokens[KSYM_MAX_TOKENS];
    int tokenCount = 0;
    bpeCompress(seqs, n, tokens, &tokenCount);

    /* Build the stream (per-symbol ULEB128 delta + encLen byte + encLen bytes) and the block
     * index (one entry per 64 symbols) together, in one left-to-right pass. */
    ByteBuf stream;
    bufInit(&stream);
    ByteBuf index;
    bufInit(&index);
    uint64_t prevAddr = 0;
    uint32_t indexCount = 0;
    for (size_t i = 0; i < n; i++) {
        if (i % KSYM_BLOCK_SIZE == 0) {
            bufU64(&index, syms->syms[i].addr);
            bufU32(&index, (uint32_t)stream.len);
            bufU32(&index, 0); /* reserved */
            indexCount++;
        }
        uint64_t delta = (i % KSYM_BLOCK_SIZE == 0) ? 0 : syms->syms[i].addr - prevAddr;
        bufUleb128(&stream, delta);
        if (seqs[i].len > 255) {
            die("a compressed symbol name exceeds 255 bytes");
        }
        bufU8(&stream, (uint8_t)seqs[i].len);
        bufBytes(&stream, seqs[i].bytes, seqs[i].len);
        prevAddr = syms->syms[i].addr;
    }

    /* Token directory: {poolOff u16, len u8, reserved u8} per token, then the concatenated pool. */
    ByteBuf tokenDir;
    bufInit(&tokenDir);
    ByteBuf tokenPool;
    bufInit(&tokenPool);
    for (int t = 0; t < tokenCount; t++) {
        bufU16(&tokenDir, (uint16_t)tokenPool.len);
        bufU8(&tokenDir, tokens[t].len);
        bufU8(&tokenDir, 0);
        bufBytes(&tokenPool, tokens[t].expansion, tokens[t].len);
    }

    uint32_t indexOffset = KSYM_HEADER_SIZE;
    uint32_t tokenOffset = indexOffset + (uint32_t)index.len;
    uint32_t streamOffset = tokenOffset + (uint32_t)tokenDir.len + (uint32_t)tokenPool.len;

    ByteBuf out;
    bufInit(&out);
    bufU32(&out, KSYM_MAGIC);
    bufU16(&out, KSYM_VERSION);
    bufU16(&out, KSYM_HEADER_SIZE);
    bufU32(&out, (uint32_t)n);
    bufU32(&out, KSYM_BLOCK_SHIFT);
    bufU64(&out, textBase);
    bufU64(&out, textEnd);
    bufU32(&out, indexOffset);
    bufU32(&out, indexCount);
    bufU32(&out, tokenOffset);
    bufU32(&out, (uint32_t)tokenCount);
    bufU32(&out, streamOffset);
    bufU32(&out, (uint32_t)stream.len);
    bufU64(&out, 0); /* reserved */
    bufBytes(&out, index.data, index.len);
    bufBytes(&out, tokenDir.data, tokenDir.len);
    bufBytes(&out, tokenPool.data, tokenPool.len);
    bufBytes(&out, stream.data, stream.len);

    free(index.data);
    free(tokenDir.data);
    free(tokenPool.data);
    free(stream.data);
    for (size_t i = 0; i < n; i++) {
        free(seqs[i].bytes);
    }
    free(seqs);

    KsymsBlob blob = {out.data, out.len};
    return blob;
}

void ksymsBlobFree(KsymsBlob *blob) {
    free(blob->data);
    blob->data = NULL;
    blob->size = 0;
}
