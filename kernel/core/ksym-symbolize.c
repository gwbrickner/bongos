/* See ksym.h. ksymSymbolize() itself -- split from ksym.c's pure/host-testable ksymDecodeLookup()
 * because this needs the kernel's own linked-in KSYM v1 blob (ksymsStart/ksymsEnd, kernel.ld,
 * D-075) and ksnprintf(), neither of which a host test binary can resolve. */
#include "ksym.h"

#include "format.h"
#include "sections.h"

void ksymSymbolize(uint64_t addr, char *buf, size_t bufSize) {
    char name[128];
    uint64_t symAddr;
    size_t blobSize = (size_t)(ksymsEnd - ksymsStart);
    Status st = ksymDecodeLookup(ksymsStart, blobSize, addr, name, sizeof(name), &symAddr);
    if (st != STATUS_OK) {
        ksnprintf(buf, bufSize, "?");
        return;
    }
    ksnprintf(buf, bufSize, "%s+0x%llx", name, (unsigned long long)(addr - symAddr));
}
