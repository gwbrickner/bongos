/* KSYM v1 encoder (docs/specs/ksyms.md, D-075): from-scratch byte-pair-token compression of the
 * kernel's own STT_FUNC symbol names, plus the header/block-index/stream serialization. Host tool
 * only -- kernel/core/ksym.c has the (much simpler) decoder. */
#ifndef TOOLS_KSYMS_KSYMS_ENCODE_H
#define TOOLS_KSYMS_KSYMS_ENCODE_H

#include "elf-read.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t size;
} KsymsBlob;

/* Encodes `syms` (already deduplicated and sorted by ascending address, as elfReadFuncSyms()
 * returns it) into a KSYM v1 blob. `textBase`/`textEnd` bound the valid lookup range (the linked
 * kernel's kernelTextStart/kernelTextEnd). Every symbol's address must satisfy
 * textBase <= addr < textEnd -- dies otherwise, since that would mean the blob's own bounds check
 * (ksymDecodeLookup) could never find it. */
KsymsBlob ksymsEncode(const ElfFuncSymList *syms, uint64_t textBase, uint64_t textEnd);

/* A valid, empty (count=0) KSYM v1 blob -- just the 64-byte header, used for the two-pass link's
 * first pass (D-075) so kernel.ld has something real to place at .ksyms before the final symbol
 * addresses are known. */
KsymsBlob ksymsEncodeEmpty(void);

void ksymsBlobFree(KsymsBlob *blob);

#endif
