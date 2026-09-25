/* Round-trips the ksyms v1 format (docs/specs/ksyms.md, D-073) through a small hand-rolled
 * encoder straight into kernel/core/ksyms-decode.c's pure decoder -- the same decoder the kernel
 * and `tools/mksyms --check` both use. */
#include "framework/test.h"
#include "ksyms-format.h"

#include <stdlib.h>
#include <string.h>

typedef struct ByteBuf {
    uint8_t *data;
    size_t len, cap;
} ByteBuf;

static void bbPush(ByteBuf *b, uint8_t v) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->data = realloc(b->data, b->cap);
    }
    b->data[b->len++] = v;
}

static void bbPushU32(ByteBuf *b, uint32_t v) {
    bbPush(b, (uint8_t)v);
    bbPush(b, (uint8_t)(v >> 8));
    bbPush(b, (uint8_t)(v >> 16));
    bbPush(b, (uint8_t)(v >> 24));
}

static void bbFree(ByteBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* Builds a valid ksyms v1 blob for `count` symbols named names[0..count), placed at offset i*16
 * with size 16 each, restart interval 16 (matching tools/mksyms' own choice). Frees any
 * previously-held buffer in *out. */
static void buildBlob(ByteBuf *out, const char *const *names, uint32_t count) {
    ByteBuf addrs = {0}, restarts = {0}, namesBuf = {0};
    char prev[256];
    prev[0] = '\0';

    for (uint32_t i = 0; i < count; i++) {
        bbPushU32(&addrs, i * 16);
        bbPushU32(&addrs, 16);

        size_t nameLen = strlen(names[i]);
        if (i % 16 == 0) { /* restartInterval == 16, always, in this test's blobs */
            bbPushU32(&restarts, (uint32_t)namesBuf.len);
            bbPush(&namesBuf, 0);
            bbPush(&namesBuf, (uint8_t)nameLen);
            for (size_t k = 0; k < nameLen; k++) {
                bbPush(&namesBuf, (uint8_t)names[i][k]);
            }
        } else {
            size_t prevLen = strlen(prev);
            size_t shared = 0, maxShared = prevLen < nameLen ? prevLen : nameLen;
            while (shared < maxShared && prev[shared] == names[i][shared]) {
                shared++;
            }
            size_t suffixLen = nameLen - shared;
            bbPush(&namesBuf, (uint8_t)shared);
            bbPush(&namesBuf, (uint8_t)suffixLen);
            for (size_t k = 0; k < suffixLen; k++) {
                bbPush(&namesBuf, (uint8_t)names[i][shared + k]);
            }
        }
        memcpy(prev, names[i], nameLen + 1);
    }
    while (namesBuf.len % 8 != 0) {
        bbPush(&namesBuf, 0);
    }

    uint32_t addrsOff = KSYMS_HEADER_SIZE;
    uint32_t restartsOff = (uint32_t)(addrsOff + addrs.len);
    uint32_t namesOff = (uint32_t)(restartsOff + restarts.len);

    if (out->data != NULL) {
        bbFree(out);
    }
    bbPushU32(out, KSYMS_MAGIC);
    bbPush(out, (uint8_t)KSYMS_VERSION);
    bbPush(out, 0);
    bbPush(out, (uint8_t)KSYMS_HEADER_SIZE);
    bbPush(out, 0);
    bbPushU32(out, count);
    bbPushU32(out, 16); /* restartInterval */
    bbPushU32(out, addrsOff);
    bbPushU32(out, restartsOff);
    bbPushU32(out, namesOff);
    bbPushU32(out, (uint32_t)namesBuf.len);
    for (size_t k = 0; k < addrs.len; k++) {
        bbPush(out, addrs.data[k]);
    }
    for (size_t k = 0; k < restarts.len; k++) {
        bbPush(out, restarts.data[k]);
    }
    for (size_t k = 0; k < namesBuf.len; k++) {
        bbPush(out, namesBuf.data[k]);
    }
    bbFree(&addrs);
    bbFree(&restarts);
    bbFree(&namesBuf);
}

static const char *genName(uint32_t i, char *buf, size_t bufSize) {
    /* Shares a "kernelSubsysFn" prefix across many entries (like real bongOS symbols do), with a
     * distinct numeric suffix -- exercises front coding across restart boundaries. */
    snprintf(buf, bufSize, "kernelSubsysFn%u", i);
    return buf;
}

TEST(ksymsEmptyTableNotFound) {
    ByteBuf blob = {0};
    const char *names[1];
    buildBlob(&blob, names, 0);

    KsymsSymbol sym;
    ASSERT_EQ(ksymsLookup(blob.data, blob.len, 0, &sym), STATUS_ERR_NOT_FOUND);
    bbFree(&blob);
}

TEST(ksymsSingleSymbol) {
    ByteBuf blob = {0};
    const char *names[1] = {"onlySymbol"};
    buildBlob(&blob, names, 1);

    KsymsSymbol sym;
    ASSERT_EQ(ksymsLookup(blob.data, blob.len, 0, &sym), STATUS_OK);
    ASSERT_STREQ(sym.name, "onlySymbol");
    ASSERT_EQ(sym.offset, 0u);
    ASSERT_EQ(sym.size, 16u);

    ASSERT_EQ(ksymsLookup(blob.data, blob.len, 15, &sym), STATUS_OK);
    ASSERT_EQ(ksymsLookup(blob.data, blob.len, 16, &sym), STATUS_ERR_NOT_FOUND);
    bbFree(&blob);
}

static void checkCount(uint32_t count) {
    ByteBuf blob = {0};
    char *names[64];
    char bufs[64][32];
    for (uint32_t i = 0; i < count; i++) {
        genName(i, bufs[i], sizeof(bufs[i]));
        names[i] = bufs[i];
    }
    buildBlob(&blob, (const char *const *)names, count);

    for (uint32_t i = 0; i < count; i++) {
        KsymsSymbol sym;
        ASSERT_EQ(ksymsLookup(blob.data, blob.len, i * 16, &sym), STATUS_OK);
        ASSERT_STREQ(sym.name, names[i]);
        ASSERT_EQ(sym.offset, i * 16);
        ASSERT_EQ(sym.size, 16u);

        /* Mid-range address inside the same symbol's [offset, offset+size). */
        ASSERT_EQ(ksymsLookup(blob.data, blob.len, i * 16 + 8, &sym), STATUS_OK);
        ASSERT_STREQ(sym.name, names[i]);
    }

    KsymsSymbol unused;
    ASSERT_EQ(ksymsLookup(blob.data, blob.len, (uint64_t)count * 16, &unused),
              STATUS_ERR_NOT_FOUND); /* past the last symbol's size */
    bbFree(&blob);
}

TEST(ksymsSixteenSymbolsExactlyOneRestart) {
    checkCount(16);
}

TEST(ksymsSeventeenSymbolsCrossesRestart) {
    checkCount(17);
}

TEST(ksymsFortySymbolsMultipleRestarts) {
    checkCount(40);
}

TEST(ksymsSharedPrefixAcrossRestartBoundary) {
    /* Entries 15 and 16 straddle a restart point (interval 16): entry 16 must NOT be front-coded
     * against entry 15, even though they share a long prefix -- it's a fresh restart entry. */
    ByteBuf blob = {0};
    const char *names[17];
    char bufs[17][32];
    for (uint32_t i = 0; i < 17; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "sharedPrefixName%u", i);
        names[i] = bufs[i];
    }
    buildBlob(&blob, names, 17);

    KsymsSymbol sym;
    ASSERT_EQ(ksymsLookup(blob.data, blob.len, 16 * 16, &sym), STATUS_OK);
    ASSERT_STREQ(sym.name, "sharedPrefixName16");
    bbFree(&blob);
}

TEST(ksymsTruncatedHeaderRejected) {
    ByteBuf blob = {0};
    const char *names[1] = {"x"};
    buildBlob(&blob, names, 1);

    KsymsSymbol sym;
    for (uint64_t len = 0; len < KSYMS_HEADER_SIZE; len++) {
        ASSERT_EQ(ksymsLookup(blob.data, len, 0, &sym), STATUS_ERR_INVALID);
    }
    bbFree(&blob);
}

TEST(ksymsCorruptMagicRejected) {
    ByteBuf blob = {0};
    const char *names[1] = {"x"};
    buildBlob(&blob, names, 1);
    blob.data[0] ^= 0xFF;

    KsymsSymbol sym;
    ASSERT_EQ(ksymsLookup(blob.data, blob.len, 0, &sym), STATUS_ERR_INVALID);
    bbFree(&blob);
}

TEST(ksymsBogusOffsetsRejected) {
    ByteBuf blob = {0};
    const char *names[2] = {"a", "b"};
    buildBlob(&blob, names, 2);
    /* Corrupt restartsOff (offset 0x14) so it no longer matches addrsOff + 8*count. */
    blob.data[0x14] ^= 0xFF;

    KsymsSymbol sym;
    ASSERT_EQ(ksymsLookup(blob.data, blob.len, 0, &sym), STATUS_ERR_INVALID);
    bbFree(&blob);
}
