/* Kernel symbol lookup (ARCHITECTURE §24, D-073). Backed by the embedded `ksyms` table
 * (docs/specs/ksyms.md) that `tools/mksyms` writes into the `.ksyms` linker section at build
 * time. */
#ifndef KERNEL_SYMBOLIZE_H
#define KERNEL_SYMBOLIZE_H

#include <stdint.h>

#include "uapi/status.h"

#define SYMBOLIZE_NAME_MAX 256

typedef struct SymbolInfo {
    char name[SYMBOLIZE_NAME_MAX];
    uint64_t start; /* absolute runtime address */
    uint64_t size;
} SymbolInfo;

/* Looks up the function symbol containing `addr`. Returns STATUS_OK and fills `*out` on a hit;
 * STATUS_ERR_NOT_FOUND if `addr` is outside [kernelTextStart, kernelTextEnd) or falls in a gap
 * the table doesn't cover. No locks, IRQ-safe, reentrant, bounded time (a header check plus a
 * bounded binary search and front-coded-name decode) -- safe to call from any trap context,
 * including NMI. */
Status symbolize(uint64_t addr, SymbolInfo *out);

/* Number of symbols in the embedded table, for the "ksyms: N symbols" boot log line. No locks. */
uint32_t symbolizeCount(void);

#endif
