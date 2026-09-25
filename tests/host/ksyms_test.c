/* Host tests for KSYM v1 (docs/specs/ksyms.md, D-075): round-trips tools/ksyms's encoder
 * (ksymsEncode) through the kernel's own decoder (ksymDecodeLookup), plus malformed-blob safety. */
#define _DEFAULT_SOURCE /* strdup (matches tests/host/imgdiff_test.c's convention) */
#include "elf-read.h"
#include "ksym.h"
#include "ksyms-encode.h"

#include "framework/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ElfFuncSym makeSym(uint64_t addr, const char *name) {
    ElfFuncSym s;
    s.addr = addr;
    s.name = strdup(name);
    return s;
}

static void testName(char *buf, size_t bufSize, int i) {
    snprintf(buf, bufSize, "sym_%d", i);
}

/* Deliberately shares long common substrings across entries (real kernel names do too --
 * ktestFn_, klogWrite, ...), so the BPE compressor has something to actually merge. */
static void testPrefixedName(char *buf, size_t bufSize, int i) {
    snprintf(buf, bufSize, "kernelDriverInitFunction_%d", i);
}

TEST(ksymsRoundTripExactAndOffsetLookups) {
    ElfFuncSym syms[4] = {
        makeSym(0xFFFFFFFF80000000ULL, "kernelEntry"),
        makeSym(0xFFFFFFFF80000010ULL, "kernelMain"),
        makeSym(0xFFFFFFFF80000100ULL, "panic"),
        makeSym(0xFFFFFFFF80001000ULL, "trapDispatch"),
    };
    ElfFuncSymList list = {syms, 4};
    KsymsBlob blob = ksymsEncode(&list, 0xFFFFFFFF80000000ULL, 0xFFFFFFFF80002000ULL);

    char name[64];
    uint64_t symAddr;

    /* Exact address match. */
    ASSERT_EQ(
        ksymDecodeLookup(blob.data, blob.size, 0xFFFFFFFF80000010ULL, name, sizeof(name), &symAddr),
        STATUS_OK);
    ASSERT_STREQ(name, "kernelMain");
    ASSERT_EQ(symAddr, 0xFFFFFFFF80000010ULL);

    /* An address a few bytes into a function's body still resolves to that function, with the
     * right offset derivable from symAddr. */
    ASSERT_EQ(
        ksymDecodeLookup(blob.data, blob.size, 0xFFFFFFFF80000123ULL, name, sizeof(name), &symAddr),
        STATUS_OK);
    ASSERT_STREQ(name, "panic");
    ASSERT_EQ(symAddr, 0xFFFFFFFF80000100ULL);

    /* The very last symbol, looked up well past its start (still within textEnd). */
    ASSERT_EQ(
        ksymDecodeLookup(blob.data, blob.size, 0xFFFFFFFF80001FFFULL, name, sizeof(name), &symAddr),
        STATUS_OK);
    ASSERT_STREQ(name, "trapDispatch");

    /* Before the first symbol, and at/after textEnd: NOT_FOUND. */
    ASSERT_EQ(
        ksymDecodeLookup(blob.data, blob.size, 0xFFFFFFFF7FFFFFFFULL, name, sizeof(name), &symAddr),
        STATUS_ERR_NOT_FOUND);
    ASSERT_EQ(
        ksymDecodeLookup(blob.data, blob.size, 0xFFFFFFFF80002000ULL, name, sizeof(name), &symAddr),
        STATUS_ERR_NOT_FOUND);

    ksymsBlobFree(&blob);
    for (int i = 0; i < 4; i++) {
        free(syms[i].name);
    }
}

TEST(ksymsRoundTripAcrossMultipleBlocks) {
    /* KSYM_BLOCK_SIZE is 64 -- 130 symbols spans 3 block-index entries, exercising the block
     * index's binary search, not just a single-block linear scan. */
    ElfFuncSym syms[130];
    for (int i = 0; i < 130; i++) {
        char name[32];
        testName(name, sizeof(name), i);
        syms[i] = makeSym(0xFFFFFFFF80000000ULL + (uint64_t)i * 16, name);
    }
    ElfFuncSymList list = {syms, 130};
    KsymsBlob blob =
        ksymsEncode(&list, 0xFFFFFFFF80000000ULL, 0xFFFFFFFF80000000ULL + 130 * 16 + 16);

    char name[64];
    uint64_t symAddr;
    for (int i = 0; i < 130; i++) {
        uint64_t addr = 0xFFFFFFFF80000000ULL + (uint64_t)i * 16 + 4; /* +4: mid-symbol lookup */
        ASSERT_EQ(ksymDecodeLookup(blob.data, blob.size, addr, name, sizeof(name), &symAddr),
                  STATUS_OK);
        char expected[32];
        testName(expected, sizeof(expected), i);
        ASSERT_STREQ(name, expected);
    }

    ksymsBlobFree(&blob);
    for (int i = 0; i < 130; i++) {
        free(syms[i].name);
    }
}

TEST(ksymDecodeLookupRejectsMalformedBlob) {
    char name[64];
    uint64_t symAddr;

    /* Too short to even hold a header. */
    uint8_t tiny[4] = {0x4B, 0x53, 0x59, 0x4D};
    ASSERT_EQ(ksymDecodeLookup(tiny, sizeof(tiny), 0x1000, name, sizeof(name), &symAddr),
              STATUS_ERR_NOT_FOUND);

    /* NULL blob. */
    ASSERT_EQ(ksymDecodeLookup(NULL, 0, 0x1000, name, sizeof(name), &symAddr),
              STATUS_ERR_NOT_FOUND);

    /* A wrong magic in an otherwise header-sized buffer. */
    uint8_t bad[64] = {0};
    bad[0] = 0xAA;
    ASSERT_EQ(ksymDecodeLookup(bad, sizeof(bad), 0x1000, name, sizeof(name), &symAddr),
              STATUS_ERR_NOT_FOUND);
}

TEST(ksymsEncodeEmptyIsAlwaysNotFound) {
    KsymsBlob blob = ksymsEncodeEmpty();
    char name[64];
    uint64_t symAddr;
    ASSERT_EQ(ksymDecodeLookup(blob.data, blob.size, 0, name, sizeof(name), &symAddr),
              STATUS_ERR_NOT_FOUND);
    ASSERT_EQ(
        ksymDecodeLookup(blob.data, blob.size, 0xFFFFFFFFFFFFFFFFULL, name, sizeof(name), &symAddr),
        STATUS_ERR_NOT_FOUND);
    ksymsBlobFree(&blob);
}

TEST(ksymsCompressionHandlesRepetitiveNames) {
    /* Real kernel symbol names share long common substrings (ktestFn_, klogWrite, ...) -- this
     * forces the BPE compressor to actually build tokens rather than trivially finding nothing to
     * merge, and confirms decoding still round-trips correctly once tokens are in play. */
    ElfFuncSym syms[20];
    for (int i = 0; i < 20; i++) {
        char name[32];
        testPrefixedName(name, sizeof(name), i);
        syms[i] = makeSym(0xFFFFFFFF80000000ULL + (uint64_t)i * 8, name);
    }
    ElfFuncSymList list = {syms, 20};
    KsymsBlob blob = ksymsEncode(&list, 0xFFFFFFFF80000000ULL, 0xFFFFFFFF80000000ULL + 20 * 8 + 8);

    char name[64];
    uint64_t symAddr;
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ(ksymDecodeLookup(blob.data, blob.size, 0xFFFFFFFF80000000ULL + (uint64_t)i * 8,
                                   name, sizeof(name), &symAddr),
                  STATUS_OK);
        char expected[32];
        testPrefixedName(expected, sizeof(expected), i);
        ASSERT_STREQ(name, expected);
    }

    ksymsBlobFree(&blob);
    for (int i = 0; i < 20; i++) {
        free(syms[i].name);
    }
}
