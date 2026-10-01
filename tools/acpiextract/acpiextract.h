/* acpiextract: parses an ACPIDUMP v1 serial log (docs/specs/acpidump.md, D-169) into tables.
 * Host tool (ARCHITECTURE §0). The parser is pure so tests/host links it; main.c is the CLI. */
#ifndef TOOLS_ACPIEXTRACT_H
#define TOOLS_ACPIEXTRACT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char sig[5]; /* the SIG4 from the TABLE line, NUL-terminated */
    uint64_t phys;
    uint32_t len;
    uint8_t *data;
} AcpiExtractTable;

typedef struct {
    uint64_t rsdp; /* the BEGIN line's rsdp= value */
    AcpiExtractTable *tables;
    uint32_t count; /* includes the RSDP block (always first) */
} AcpiExtractDump;

/* Parses the last BEGIN..END block of `text` (`len` bytes, need not be NUL-terminated). Returns 0
 * on success; otherwise -1 with a message naming the line number in `err`. The block must be
 * complete and valid: contiguous offsets, byte totals equal to len, the END count equal to the
 * number of TABLE blocks, and valid checksums (and, for non-RSDP tables, a header Length equal to
 * len and a signature equal to SIG4). On failure `*out` is empty. */
int acpiExtractParse(const char *text, size_t len, AcpiExtractDump *out, char *err, size_t errCap);

void acpiExtractFree(AcpiExtractDump *d);

#endif
