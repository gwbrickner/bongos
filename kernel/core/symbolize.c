/* See symbolize.h. Thin wrapper around ksyms-decode.c's pure decoder, binding it to the kernel's
 * own embedded `.ksyms` blob (kernel.ld) and `.text` range (sections.h). */
#include "symbolize.h"

#include "ksyms-format.h"
#include "sections.h"

Status symbolize(uint64_t addr, SymbolInfo *out) {
    uint64_t textStart = (uint64_t)(uintptr_t)kernelTextStart;
    uint64_t textEnd = (uint64_t)(uintptr_t)kernelTextEnd;
    if (addr < textStart || addr >= textEnd) {
        return STATUS_ERR_NOT_FOUND;
    }

    const uint8_t *blob = kernelKsymsStart;
    uint64_t blobLen = (uint64_t)(kernelKsymsEnd - kernelKsymsStart);

    KsymsSymbol sym;
    Status st = ksymsLookup(blob, blobLen, addr - textStart, &sym);
    if (st != STATUS_OK) {
        return st;
    }

    out->start = textStart + sym.offset;
    out->size = sym.size;
    uint32_t i = 0;
    for (; i < SYMBOLIZE_NAME_MAX - 1 && sym.name[i] != '\0'; i++) {
        out->name[i] = sym.name[i];
    }
    out->name[i] = '\0';
    return STATUS_OK;
}

uint32_t symbolizeCount(void) {
    const uint8_t *blob = kernelKsymsStart;
    uint64_t blobLen = (uint64_t)(kernelKsymsEnd - kernelKsymsStart);
    if (blobLen < KSYMS_OFF_COUNT + 4) {
        return 0;
    }
    return (uint32_t)blob[KSYMS_OFF_COUNT] | ((uint32_t)blob[KSYMS_OFF_COUNT + 1] << 8) |
           ((uint32_t)blob[KSYMS_OFF_COUNT + 2] << 16) |
           ((uint32_t)blob[KSYMS_OFF_COUNT + 3] << 24);
}
