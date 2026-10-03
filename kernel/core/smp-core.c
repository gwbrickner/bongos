/* Pure SMP selection logic (M3.5, D-193): see kernel/include/smp.h. */
#include "smp.h"

#include "cmdline.h"

uint32_t smpParseCpusOption(const char *cmdline, uint32_t cpuMax, bool *present, bool *invalid) {
    *present = false;
    *invalid = false;
    const char *v;
    size_t len;
    if (!cmdlineFindValueSpan(cmdline, "cpus", &v, &len)) {
        return cpuMax;
    }
    *present = true;
    /* Decimal, read in place and in full (a copy into a fixed buffer would truncate a long value
     * into a different number); saturates just above cpuMax, since anything larger is clamped. */
    uint64_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') {
            *invalid = true;
            return cpuMax;
        }
        n = n * 10 + (uint64_t)(v[i] - '0');
        if (n > cpuMax) {
            n = (uint64_t)cpuMax + 1;
        }
    }
    if (len == 0 || n == 0) {
        *invalid = true;
        return cpuMax;
    }
    return n > cpuMax ? cpuMax : (uint32_t)n;
}

uint32_t smpSelectAps(const AcpiCpu *cpus, uint32_t cpuCount, uint32_t bspApicId, bool bspX2apic,
                      uint32_t maxTotal, uint32_t *out, uint32_t outCap, uint32_t *skippedXapic) {
    uint32_t n = 0;
    *skippedXapic = 0;
    for (uint32_t i = 0; i < cpuCount; i++) {
        if (n + 1 >= maxTotal || n >= outCap) {
            break;
        }
        if ((cpus[i].flags & 1u) == 0 || cpus[i].apicId == bspApicId) {
            continue;
        }
        if (!bspX2apic && cpus[i].apicId > SMP_APIC_ID_XAPIC_MAX) {
            (*skippedXapic)++;
            continue;
        }
        out[n++] = cpus[i].apicId;
    }
    return n;
}

bool smpPickTrampolinePage(const BootMemRegion *map, uint32_t count, uint64_t *outPhys) {
    bool found = false;
    uint64_t best = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (map[i].type != BOOT_MEM_USABLE) {
            continue;
        }
        uint64_t start = (map[i].base + 0xFFFu) & ~0xFFFull;
        uint64_t end = (map[i].base + map[i].length) & ~0xFFFull;
        if (map[i].base + map[i].length < map[i].base) {
            continue; /* overflowing region: not trusted */
        }
        if (start < 0x1000) {
            start = 0x1000;
        }
        if (end > SMP_TRAMP_LIMIT) {
            end = SMP_TRAMP_LIMIT;
        }
        if (end < start + 0x1000) {
            continue;
        }
        uint64_t p = end - 0x1000;
        if (!found || p > best) {
            best = p;
            found = true;
        }
    }
    if (found) {
        *outPhys = best;
    }
    return found;
}
