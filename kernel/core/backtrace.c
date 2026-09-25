/* See backtrace.h. */
#include "backtrace.h"

#include "format.h"
#include "klog.h"
#include "sections.h"
#include "symbolize.h"

#include <stdint.h>

#define BACKTRACE_MAX_FRAMES 32

typedef struct StackRange {
    uint64_t bottom, top;
} StackRange;

static const StackRange *findRange(uint64_t fp, const StackRange *ranges, uint32_t rangeCount) {
    for (uint32_t i = 0; i < rangeCount; i++) {
        /* Both saved-rbp and return-address words (16 bytes) must lie fully inside the range. */
        if (fp >= ranges[i].bottom && fp <= ranges[i].top - 16) {
            return &ranges[i];
        }
    }
    return NULL;
}

static uint32_t walk(uint64_t fp, uint64_t *pcs, uint32_t max) {
    StackRange ranges[4] = {
        {(uint64_t)(uintptr_t)kernelBootStackBottom, (uint64_t)(uintptr_t)kernelBootStackTop},
        {(uint64_t)(uintptr_t)kernelIst1StackBottom, (uint64_t)(uintptr_t)kernelIst1StackTop},
        {(uint64_t)(uintptr_t)kernelIst2StackBottom, (uint64_t)(uintptr_t)kernelIst2StackTop},
        {(uint64_t)(uintptr_t)kernelIst3StackBottom, (uint64_t)(uintptr_t)kernelIst3StackTop},
    };

    uint32_t n = 0;
    const StackRange *curRange = NULL;
    uint64_t lastFp = 0;
    while (n < max && fp != 0) {
        if ((fp & 7) != 0) {
            break;
        }
        const StackRange *r = findRange(fp, ranges, 4);
        if (r == NULL) {
            break;
        }
        if (r == curRange && fp <= lastFp) {
            break; /* must strictly increase within the same stack range */
        }
        curRange = r;
        lastFp = fp;

        const uint64_t *frameWords = (const uint64_t *)(uintptr_t)fp;
        uint64_t savedFp = frameWords[0];
        uint64_t returnAddr = frameWords[1];
        pcs[n++] = returnAddr;
        fp = savedFp;
    }
    return n;
}

uint32_t backtraceCapture(uint64_t fp, uint64_t *pcs, uint32_t max) {
    return walk(fp, pcs, max);
}

static void printFrame(uint32_t frameNo, uint64_t pc, uint64_t lookupPc) {
    char line[112];
    SymbolInfo sym;
    if (symbolize(lookupPc, &sym) == STATUS_OK) {
        ksnprintf(line, sizeof(line), "  #%u 0x%016llx %s+0x%llx/0x%llx\n", frameNo, pc, sym.name,
                  pc - sym.start, sym.size);
    } else {
        ksnprintf(line, sizeof(line), "  #%u 0x%016llx ?\n", frameNo, pc);
    }
    klogRaw(line);
}

void backtracePrint(uint64_t faultPc, uint64_t fp) {
    printFrame(0, faultPc, faultPc);

    uint64_t pcs[BACKTRACE_MAX_FRAMES - 1];
    uint32_t n = walk(fp, pcs, BACKTRACE_MAX_FRAMES - 1);
    for (uint32_t i = 0; i < n; i++) {
        printFrame(i + 1, pcs[i], pcs[i] - 1);
    }
}
