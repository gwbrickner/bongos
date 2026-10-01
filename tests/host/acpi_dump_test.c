/* Host tests for the ACPIDUMP v1 formatter (kernel/drivers/acpi/acpi-dump.c) and
 * tools/acpiextract's parser (M3.1, D-169): a synthetic AcpiTableSet round-trips byte for byte, and
 * malformed logs are rejected with a line number. */
#include "acpi-dump.h"
#include "acpiextract.h"
#include "framework/test.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text;
    size_t len, cap;
} Sink;

static void sinkLine(void *ctx, const char *line) {
    Sink *s = ctx;
    size_t n = strlen(line);
    while (s->len + n + 3 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->text = realloc(s->text, s->cap);
    }
    memcpy(s->text + s->len, line, n);
    s->len += n;
    s->text[s->len++] = '\r'; /* the UART path writes \r\n */
    s->text[s->len++] = '\n';
}

static void mk(uint8_t *t, const char *sig, uint32_t len) {
    memset(t, 0, len);
    memcpy(t, sig, 4);
    t[4] = (uint8_t)len;
    t[5] = (uint8_t)(len >> 8);
    for (uint32_t i = 36; i < len; i++) {
        t[i] = (uint8_t)(i * 7 + len);
    }
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + t[i]);
    }
    t[9] = (uint8_t)(0 - sum);
}

static AcpiTableSet *makeSet(uint8_t **bufs) {
    AcpiTableSet *s = calloc(1, sizeof(*s));
    s->rsdpPhys = 0xF5A30;
    s->rsdpLength = 20;
    memcpy(s->rsdpRaw, "RSD PTR ", 8);
    s->rsdpRaw[15] = 0;
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) {
        sum = (uint8_t)(sum + s->rsdpRaw[i]);
    }
    s->rsdpRaw[8] = (uint8_t)(0 - sum);
    static const char *sigs[] = {"RSDT", "FACP", "APIC", "S\x01SD"};
    static const uint32_t lens[] = {44, 244, 100, 36};
    for (int i = 0; i < 4; i++) {
        bufs[i] = malloc(lens[i]);
        mk(bufs[i], sigs[i], lens[i]);
        AcpiTable *t = &s->tables[s->count++];
        memcpy(t->signature, sigs[i], 4);
        t->length = lens[i];
        t->phys = 0x1000u * (uint32_t)(i + 1);
        t->data = bufs[i];
    }
    return s;
}

TEST(acpiDumpRoundTrip) {
    uint8_t *bufs[4];
    AcpiTableSet *s = makeSet(bufs);
    Sink sink = {0};
    acpiDumpTables(s, sinkLine, &sink);
    /* Noise before and inside the block must be ignored. */
    char *noisy = malloc(sink.len + 64);
    memcpy(noisy, "[info] boot: hello\r\n", 20);
    memcpy(noisy + 20, sink.text, sink.len);
    AcpiExtractDump d;
    char err[256];
    ASSERT_EQ(acpiExtractParse(noisy, sink.len + 20, &d, err, sizeof(err)), 0);
    ASSERT_EQ(d.count, 5u);
    ASSERT_EQ(d.rsdp, (uint64_t)0xF5A30);
    ASSERT_TRUE(strcmp(d.tables[0].sig, "RSDP") == 0);
    ASSERT_EQ(d.tables[0].len, 20u);
    ASSERT_TRUE(memcmp(d.tables[0].data, s->rsdpRaw, 20) == 0);
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_EQ(d.tables[i + 1].len, s->tables[i].length);
        ASSERT_EQ(d.tables[i + 1].phys, s->tables[i].phys);
        ASSERT_TRUE(memcmp(d.tables[i + 1].data, bufs[i], s->tables[i].length) == 0);
    }
    ASSERT_TRUE(strcmp(d.tables[4].sig, "S_SD") == 0); /* non-alphanumeric byte sanitized */
    acpiExtractFree(&d);
    free(noisy);
    free(sink.text);
    for (int i = 0; i < 4; i++) {
        free(bufs[i]);
    }
    free(s);
}

/* Applies `mutate` to a fresh dump and expects the parse to fail with a message mentioning "line".
 */
static void expectRejected(const char *from, const char *to) {
    uint8_t *bufs[4];
    AcpiTableSet *s = makeSet(bufs);
    Sink sink = {0};
    acpiDumpTables(s, sinkLine, &sink);
    sink.text[sink.len] = '\0';
    char *pos = strstr(sink.text, from);
    ASSERT_TRUE(pos != NULL);
    size_t fl = strlen(from), tl = strlen(to);
    char *out = malloc(sink.len + tl + 1);
    size_t head = (size_t)(pos - sink.text);
    memcpy(out, sink.text, head);
    memcpy(out + head, to, tl);
    memcpy(out + head + tl, pos + fl, sink.len - head - fl + 1);
    AcpiExtractDump d;
    char err[256];
    ASSERT_EQ(acpiExtractParse(out, strlen(out), &d, err, sizeof(err)), -1);
    ASSERT_EQ(d.count, 0u);
    ASSERT_TRUE(strstr(err, "line ") != NULL);
    free(out);
    free(sink.text);
    for (int i = 0; i < 4; i++) {
        free(bufs[i]);
    }
    free(s);
}

TEST(acpiExtractRejectsMalformed) {
    expectRejected("ACPIDUMP END tables=5", "ACPIDUMP END tables=6");
    expectRejected("len=244", "len=245");                     /* byte total != len */
    expectRejected("ACPIDUMP 00000020", "ACPIDUMP 00000040"); /* offsets not contiguous */
    expectRejected("TABLE-END FACP", "TABLE-END APIC");
    expectRejected("TABLE APIC", "TABLE APIX"); /* SIG4 != the table's own signature */
    expectRejected("ACPIDUMP 00000000 4641", "ACPIDUMP 00000000 4642"); /* checksum */
    expectRejected("phys=0x0000000000001000 len=44", "phys=0x00000000000010 len=44");
    expectRejected("ACPIDUMP BEGIN v1 rsdp=0x", "ACPIDUMP BEGIN v1 rsdp=0xZ");
}

TEST(acpiExtractNeedsCompleteBlock) {
    uint8_t *bufs[4];
    AcpiTableSet *s = makeSet(bufs);
    Sink sink = {0};
    acpiDumpTables(s, sinkLine, &sink);
    AcpiExtractDump d;
    char err[256];
    /* Truncated log: drop the END line. */
    size_t cut = sink.len;
    while (cut > 0 && sink.text[cut - 2] != '\n') {
        cut--;
    }
    ASSERT_EQ(acpiExtractParse(sink.text, cut - 0, &d, err, sizeof(err)), -1);
    ASSERT_TRUE(strstr(err, "no END") != NULL);
    ASSERT_EQ(acpiExtractParse("nothing here\n", 13, &d, err, sizeof(err)), -1);
    ASSERT_EQ(acpiExtractParse("", 0, &d, err, sizeof(err)), -1);
    /* Two blocks: the last one wins. */
    char *two = malloc(sink.len * 2);
    memcpy(two, sink.text, sink.len);
    memcpy(two + sink.len, sink.text, sink.len);
    ASSERT_EQ(acpiExtractParse(two, sink.len * 2, &d, err, sizeof(err)), 0);
    ASSERT_EQ(d.count, 5u);
    acpiExtractFree(&d);
    free(two);
    free(sink.text);
    for (int i = 0; i < 4; i++) {
        free(bufs[i]);
    }
    free(s);
}
