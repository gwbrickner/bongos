/* Reads STT_FUNC symbols in executable sections out of a linked ELF64 executable (D-075). Host
 * tool only -- this is a from-scratch minimal ELF64 section-header/symtab reader, not a general
 * ELF library, matching this project's "no third-party code" rule (ARCHITECTURE §0). */
#ifndef TOOLS_KSYMS_ELF_READ_H
#define TOOLS_KSYMS_ELF_READ_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t addr;
    char *name; /* owned, NUL-terminated, 7-bit ASCII (checked) */
} ElfFuncSym;

typedef struct {
    ElfFuncSym *syms;
    size_t count;
} ElfFuncSymList;

/* Parses `path` (a linked ELF64 executable) and returns every STT_FUNC symbol defined in a
 * section with SHF_EXECINSTR set, deduplicated by address (STB_GLOBAL wins over STB_LOCAL/WEAK;
 * a further tie breaks on the lexicographically smallest name) and sorted by ascending address.
 * Dies with a message on any malformed input -- this only ever runs on a kernel.elf this same
 * build just linked, so a parse failure here means a real bug, not untrusted input. */
ElfFuncSymList elfReadFuncSyms(const char *path);

void elfFuncSymListFree(ElfFuncSymList *list);

/* Looks up a single symbol (of any type -- kernelTextStart/kernelTextEnd are linker-script
 * symbols, STT_NOTYPE) by exact name. Returns 1 and sets *outValue if found, 0 otherwise. */
int elfReadSymbolValue(const char *path, const char *name, uint64_t *outValue);

/* Reads a named section's raw bytes (e.g. ".ksyms") into a newly malloc'd buffer. Returns 1 and
 * sets outData/outSize if the section exists (even if empty), 0 if there's no section with that
 * name. Caller frees outData. */
int elfReadSection(const char *path, const char *sectionName, uint8_t **outData, size_t *outSize);

#endif
