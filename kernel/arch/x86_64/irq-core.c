/* See irq-core.h. */
#include "irq-core.h"

#include <stddef.h>

void irqVectorMapInit(IrqVectorMap *m) {
    for (uint32_t i = 0; i < 4; i++) {
        m->used[i] = 0;
    }
    for (uint32_t v = 0; v < IRQ_VECTOR_FIRST_DYNAMIC; v++) {
        m->used[v / 64] |= 1ULL << (v % 64);
    }
    for (uint32_t v = IRQ_VECTOR_LAST_DYNAMIC + 1; v < IRQ_VECTOR_COUNT; v++) {
        m->used[v / 64] |= 1ULL << (v % 64);
    }
}

Status irqVectorMapAlloc(IrqVectorMap *m, uint32_t *out) {
    for (uint32_t v = IRQ_VECTOR_FIRST_DYNAMIC; v <= IRQ_VECTOR_LAST_DYNAMIC; v++) {
        if ((m->used[v / 64] & (1ULL << (v % 64))) == 0) {
            m->used[v / 64] |= 1ULL << (v % 64);
            *out = v;
            return STATUS_OK;
        }
    }
    return STATUS_ERR_NO_MEMORY;
}

Status irqVectorMapFree(IrqVectorMap *m, uint32_t v) {
    if (!irqVectorMapIsAllocated(m, v)) {
        return STATUS_ERR_INVALID;
    }
    m->used[v / 64] &= ~(1ULL << (v % 64));
    return STATUS_OK;
}

bool irqVectorMapIsAllocated(const IrqVectorMap *m, uint32_t v) {
    if (v < IRQ_VECTOR_FIRST_DYNAMIC || v > IRQ_VECTOR_LAST_DYNAMIC) {
        return false;
    }
    return (m->used[v / 64] & (1ULL << (v % 64))) != 0;
}

uint32_t irqVectorMapAllocatedCount(const IrqVectorMap *m) {
    uint32_t n = 0;
    for (uint32_t v = IRQ_VECTOR_FIRST_DYNAMIC; v <= IRQ_VECTOR_LAST_DYNAMIC; v++) {
        n += (m->used[v / 64] >> (v % 64)) & 1;
    }
    return n;
}

typedef struct {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} AcceptedIso;

Status irqCoreIsaRoute(const AcpiIso *isos, uint32_t n, uint32_t irq, IrqIsaRoute *out,
                       bool *outWarn) {
    if (out == NULL || (isos == NULL && n != 0) || irq >= IRQ_ISA_COUNT) {
        return STATUS_ERR_INVALID;
    }
    bool warn = false;
    AcceptedIso acc[IRQ_ISA_COUNT];
    uint32_t accCount = 0;
    for (uint32_t i = 0; i < n; i++) {
        const AcpiIso *o = &isos[i];
        if (o->bus != 0 || o->source >= IRQ_ISA_COUNT) {
            warn = true;
            continue;
        }
        bool dup = false;
        for (uint32_t j = 0; j < accCount; j++) {
            dup = dup || acc[j].source == o->source || acc[j].gsi == o->gsi;
        }
        if (dup) {
            warn = true;
            continue;
        }
        acc[accCount].source = o->source;
        acc[accCount].gsi = o->gsi;
        acc[accCount].flags = o->flags;
        accCount++;
    }

    uint32_t gsi = irq;
    uint16_t flags = 0;
    bool found = false;
    for (uint32_t j = 0; j < accCount; j++) {
        if (acc[j].source == irq) {
            gsi = acc[j].gsi;
            flags = acc[j].flags;
            found = true;
        }
    }
    if (!found) {
        for (uint32_t j = 0; j < accCount; j++) {
            if (acc[j].gsi == irq) {
                if (outWarn != NULL) {
                    *outWarn = warn;
                }
                return STATUS_ERR_NOT_FOUND;
            }
        }
    }

    bool activeLow = false; /* ISA bus default: active high, edge */
    bool level = false;
    uint32_t pol = flags & 3u;
    uint32_t trig = (flags >> 2) & 3u;
    if (pol == 3) {
        activeLow = true;
    } else if (pol == 2) {
        warn = true;
    }
    if (trig == 3) {
        level = true;
    } else if (trig == 2) {
        warn = true;
    }
    out->gsi = gsi;
    out->activeLow = activeLow;
    out->level = level;
    if (outWarn != NULL) {
        *outWarn = warn;
    }
    return STATUS_OK;
}

uint64_t irqCoreRteEncode(uint8_t vector, bool activeLow, bool level, bool masked, uint8_t dest) {
    uint64_t e = vector;
    if (activeLow) {
        e |= 1ULL << 13;
    }
    if (level) {
        e |= 1ULL << 15;
    }
    if (masked) {
        e |= 1ULL << 16;
    }
    e |= (uint64_t)dest << 56;
    return e;
}

uint32_t irqCoreIoapicPins(uint32_t versionReg) {
    return ((versionReg >> 16) & 0xFFu) + 1u;
}

int32_t irqCoreGsiLookup(const IrqGsiRange *r, uint32_t n, uint32_t gsi, uint32_t *outPin) {
    for (uint32_t i = 0; i < n; i++) {
        if (gsi >= r[i].gsiBase && (uint64_t)gsi < (uint64_t)r[i].gsiBase + r[i].pins) {
            if (outPin != NULL) {
                *outPin = gsi - r[i].gsiBase;
            }
            return (int32_t)i;
        }
    }
    return -1;
}

bool irqCoreGsiOverlaps(const IrqGsiRange *r, uint32_t n, IrqGsiRange cand) {
    if (cand.pins == 0) {
        return true;
    }
    uint64_t cLo = cand.gsiBase;
    uint64_t cHi = cLo + cand.pins;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t lo = r[i].gsiBase;
        uint64_t hi = lo + r[i].pins;
        if (cLo < hi && lo < cHi) {
            return true;
        }
    }
    return false;
}
