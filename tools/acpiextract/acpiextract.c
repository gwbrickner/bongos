/* acpiextract's ACPIDUMP v1 parser (docs/specs/acpidump.md). Written to survive any input: every
 * length is bounded before it sizes an allocation, and nothing trusts the log's own counts. */
#include "acpiextract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXTRACT_MAX_TABLE  0x400000u
#define EXTRACT_MAX_TABLES 1024u

typedef struct {
    AcpiExtractTable *tables;
    uint32_t count, cap;
    uint8_t *cur; /* the table being filled */
    uint32_t curLen, curFill;
    int inTable;
    uint64_t rsdp;
    int open;     /* between BEGIN and END */
    int complete; /* END seen and verified */
    uint32_t endCount;
} Block;

static void blockFree(Block *b) {
    for (uint32_t i = 0; i < b->count; i++) {
        free(b->tables[i].data);
    }
    free(b->tables);
    free(b->cur);
    memset(b, 0, sizeof(*b));
}

static void setErr(char *err, size_t cap, size_t lineNo, const char *msg) {
    if (cap != 0) {
        snprintf(err, cap, "line %zu: %s", lineNo, msg);
    }
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Parses exactly `digits` lowercase hex digits at *p into *v and advances. */
static int parseHex(const char **p, const char *end, int digits, uint64_t *v) {
    *v = 0;
    for (int i = 0; i < digits; i++) {
        if (*p >= end || hexVal(**p) < 0) {
            return -1;
        }
        *v = (*v << 4) | (uint64_t)hexVal(**p);
        (*p)++;
    }
    return 0;
}

static int parseDec(const char **p, const char *end, uint64_t *v) {
    *v = 0;
    int any = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        *v = *v * 10 + (uint64_t)(**p - '0');
        if (*v > 0xFFFFFFFFull) {
            return -1;
        }
        any = 1;
        (*p)++;
    }
    return any ? 0 : -1;
}

static int lit(const char **p, const char *end, const char *s) {
    size_t n = strlen(s);
    if ((size_t)(end - *p) < n || memcmp(*p, s, n) != 0) {
        return -1;
    }
    *p += n;
    return 0;
}

static uint8_t sum8(const uint8_t *d, uint32_t n) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) {
        s = (uint8_t)(s + d[i]);
    }
    return s;
}

static int sigOk(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* Verifies a finished table's content against its TABLE line. Returns a message or NULL. */
static const char *verifyTable(const AcpiExtractTable *t, int isRsdp) {
    if (isRsdp) {
        if (t->len < 20 || memcmp(t->data, "RSD PTR ", 8) != 0) {
            return "RSDP block is not an RSDP";
        }
        if (sum8(t->data, 20) != 0) {
            return "RSDP checksum is wrong";
        }
        if (t->len >= 36 && sum8(t->data, t->len) != 0) {
            return "RSDP extended checksum is wrong";
        }
        return NULL;
    }
    if (t->len < 36) {
        return "table shorter than an ACPI header";
    }
    for (int i = 0; i < 4; i++) {
        char want = sigOk((char)t->data[i]) ? (char)t->data[i] : '_';
        if (t->sig[i] != want) {
            return "SIG4 does not match the table's own signature";
        }
    }
    uint32_t hl = (uint32_t)t->data[4] | ((uint32_t)t->data[5] << 8) |
                  ((uint32_t)t->data[6] << 16) | ((uint32_t)t->data[7] << 24);
    if (hl != t->len) {
        return "header Length does not equal len";
    }
    if (sum8(t->data, t->len) != 0) {
        return "table checksum is wrong";
    }
    return NULL;
}

int acpiExtractParse(const char *text, size_t len, AcpiExtractDump *out, char *err, size_t errCap) {
    memset(out, 0, sizeof(*out));
    if (errCap != 0) {
        err[0] = '\0';
    }
    Block b;
    memset(&b, 0, sizeof(b));
    int sawBegin = 0;
    const char *end = text + len;
    size_t lineNo = 0;
    const char *p = text;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *lineEnd = nl != NULL ? nl : end;
        const char *next = nl != NULL ? nl + 1 : end;
        lineNo++;
        const char *q = p;
        const char *le = lineEnd;
        if (le > q && le[-1] == '\r') {
            le--;
        }
        p = next;
        if (lit(&q, le, "ACPIDUMP ") != 0) {
            continue; /* not ours, ignored even inside a block */
        }
        const char *bad = NULL;
        if (lit(&q, le, "BEGIN v1 rsdp=0x") == 0) {
            blockFree(&b); /* the last BEGIN wins */
            sawBegin = 1;
            b.open = 1;
            if (parseHex(&q, le, 16, &b.rsdp) != 0 || q != le) {
                bad = "malformed BEGIN line";
            }
        } else if (!b.open) {
            continue; /* a stray line of a block we never saw begin: ignore until BEGIN */
        } else if (lit(&q, le, "TABLE-END ") == 0) {
            if (!b.inTable) {
                bad = "TABLE-END without TABLE";
            } else if (b.curFill != b.curLen) {
                bad = "table byte total does not equal len";
            } else {
                AcpiExtractTable *t = &b.tables[b.count - 1];
                t->data = b.cur;
                b.cur = NULL;
                b.inTable = 0;
                bad = verifyTable(t, b.count == 1);
                if (bad == NULL && ((size_t)(le - q) != 4 || memcmp(q, t->sig, 4) != 0)) {
                    bad = "TABLE-END signature does not match TABLE";
                }
            }
        } else if (lit(&q, le, "TABLE ") == 0) {
            if (b.inTable) {
                bad = "TABLE inside an unfinished table";
            } else if (b.count >= EXTRACT_MAX_TABLES) {
                bad = "too many tables";
            } else {
                AcpiExtractTable t;
                memset(&t, 0, sizeof(t));
                uint64_t phys, tlen;
                if (le - q < 5) {
                    bad = "malformed TABLE line";
                } else {
                    memcpy(t.sig, q, 4);
                    q += 4;
                    if (lit(&q, le, " phys=0x") != 0 || parseHex(&q, le, 16, &phys) != 0 ||
                        lit(&q, le, " len=") != 0 || parseDec(&q, le, &tlen) != 0 || q != le) {
                        bad = "malformed TABLE line";
                    } else if (tlen == 0 || tlen > EXTRACT_MAX_TABLE) {
                        bad = "table len out of range";
                    } else if ((b.count == 0) != (strcmp(t.sig, "RSDP") == 0)) {
                        bad = "the RSDP block must come first, and only first";
                    } else {
                        if (b.count == b.cap) {
                            uint32_t ncap = b.cap == 0 ? 16 : b.cap * 2;
                            AcpiExtractTable *nt = realloc(b.tables, ncap * sizeof(*nt));
                            if (nt == NULL) {
                                blockFree(&b);
                                setErr(err, errCap, lineNo, "out of memory");
                                return -1;
                            }
                            b.tables = nt;
                            b.cap = ncap;
                        }
                        t.phys = phys;
                        t.len = (uint32_t)tlen;
                        b.cur = malloc(t.len);
                        if (b.cur == NULL) {
                            blockFree(&b);
                            setErr(err, errCap, lineNo, "out of memory");
                            return -1;
                        }
                        b.curLen = t.len;
                        b.curFill = 0;
                        b.inTable = 1;
                        b.tables[b.count++] = t;
                    }
                }
            }
        } else if (lit(&q, le, "END tables=") == 0) {
            uint64_t n;
            if (b.inTable) {
                bad = "END inside an unfinished table";
            } else if (parseDec(&q, le, &n) != 0 || q != le) {
                bad = "malformed END line";
            } else if (n != b.count) {
                bad = "END table count does not match the number of TABLE blocks";
            } else if (b.count == 0) {
                bad = "no tables";
            } else {
                b.complete = 1;
                b.open = 0;
            }
        } else {
            /* A data line: <8 hex offset> <hex bytes> */
            uint64_t off;
            if (!b.inTable) {
                bad = "data line outside a table";
            } else if (parseHex(&q, le, 8, &off) != 0 || q >= le || *q != ' ') {
                bad = "malformed data line";
            } else {
                q++;
                size_t digits = (size_t)(le - q);
                if (digits == 0 || digits > 64 || (digits & 1) != 0) {
                    bad = "data line has a bad byte count";
                } else if (off != b.curFill) {
                    bad = "data offsets are not contiguous from 0";
                } else if (b.curFill + digits / 2 > b.curLen) {
                    bad = "table data exceeds len";
                } else {
                    for (size_t i = 0; i < digits / 2; i++) {
                        uint64_t v;
                        if (parseHex(&q, le, 2, &v) != 0) {
                            bad = "bad hex in data line";
                            break;
                        }
                        b.cur[b.curFill++] = (uint8_t)v;
                    }
                }
            }
        }
        if (bad != NULL) {
            blockFree(&b);
            setErr(err, errCap, lineNo, bad);
            return -1;
        }
    }
    if (!sawBegin) {
        setErr(err, errCap, lineNo, "no ACPIDUMP block in the log");
        return -1;
    }
    if (!b.complete) {
        blockFree(&b);
        setErr(err, errCap, lineNo, "the last ACPIDUMP block has no END (truncated log?)");
        return -1;
    }
    out->rsdp = b.rsdp;
    out->tables = b.tables;
    out->count = b.count;
    b.tables = NULL;
    b.count = 0;
    blockFree(&b);
    return 0;
}

void acpiExtractFree(AcpiExtractDump *d) {
    for (uint32_t i = 0; i < d->count; i++) {
        free(d->tables[i].data);
    }
    free(d->tables);
    memset(d, 0, sizeof(*d));
}
