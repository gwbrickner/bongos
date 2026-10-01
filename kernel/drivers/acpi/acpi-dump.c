/* ACPIDUMP v1 formatter (D-169). Hand-rolled hex/decimal so the file has no libc or kernel-printf
 * dependency and builds identically in the kernel and in the host tests. */
#include "acpi-dump.h"

#include <stdbool.h>

#define DUMP_BYTES_PER_LINE 32u
#define DUMP_LINE_MAX       128

typedef struct {
    char buf[DUMP_LINE_MAX];
    size_t n;
} Line;

static void putc_(Line *l, char c) {
    if (l->n < DUMP_LINE_MAX - 1) {
        l->buf[l->n++] = c;
    }
}

static void puts_(Line *l, const char *s) {
    while (*s != '\0') {
        putc_(l, *s++);
    }
}

static void putHex(Line *l, uint64_t v, int digits) {
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        putc_(l, hex[(v >> (4 * i)) & 0xF]);
    }
}

static void putDec(Line *l, uint32_t v) {
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        putc_(l, tmp[--n]);
    }
}

static void flush(Line *l, AcpiDumpEmit emit, void *ctx) {
    l->buf[l->n] = '\0';
    emit(ctx, l->buf);
    l->n = 0;
}

static void putSig(Line *l, const uint8_t *sig) {
    for (int i = 0; i < 4; i++) {
        uint8_t c = sig[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        putc_(l, ok ? (char)c : '_');
    }
}

static void emitTable(AcpiDumpEmit emit, void *ctx, const char *sigName, const uint8_t *sig4,
                      uint64_t phys, const uint8_t *data, uint32_t len) {
    Line l = {{0}, 0};
    puts_(&l, "ACPIDUMP TABLE ");
    if (sigName != NULL) {
        puts_(&l, sigName);
    } else {
        putSig(&l, sig4);
    }
    puts_(&l, " phys=0x");
    putHex(&l, phys, 16);
    puts_(&l, " len=");
    putDec(&l, len);
    flush(&l, emit, ctx);
    for (uint32_t off = 0; off < len; off += DUMP_BYTES_PER_LINE) {
        uint32_t n = len - off < DUMP_BYTES_PER_LINE ? len - off : DUMP_BYTES_PER_LINE;
        puts_(&l, "ACPIDUMP ");
        putHex(&l, off, 8);
        putc_(&l, ' ');
        for (uint32_t i = 0; i < n; i++) {
            putHex(&l, data[off + i], 2);
        }
        flush(&l, emit, ctx);
    }
    puts_(&l, "ACPIDUMP TABLE-END ");
    if (sigName != NULL) {
        puts_(&l, sigName);
    } else {
        putSig(&l, sig4);
    }
    flush(&l, emit, ctx);
}

void acpiDumpTables(const AcpiTableSet *s, AcpiDumpEmit emit, void *ctx) {
    Line l = {{0}, 0};
    puts_(&l, "ACPIDUMP BEGIN v1 rsdp=0x");
    putHex(&l, s->rsdpPhys, 16);
    flush(&l, emit, ctx);
    emitTable(emit, ctx, "RSDP", NULL, s->rsdpPhys, s->rsdpRaw, s->rsdpLength);
    for (uint32_t i = 0; i < s->count; i++) {
        const AcpiTable *t = &s->tables[i];
        emitTable(emit, ctx, NULL, (const uint8_t *)t->signature, t->phys, t->data, t->length);
    }
    puts_(&l, "ACPIDUMP END tables=");
    putDec(&l, s->count + 1);
    flush(&l, emit, ctx);
}
