/* Pure ACPI table loader and parsers (D-166/D-167). See kernel/include/acpi-tables.h. Offsets are
 * from ACPI 6.5 (§5.2), the PCI Firmware Spec 3.x (MCFG), the IA-PC HPET spec 1.0a and AMD IOMMU
 * spec #48882 (IVRS). No libc: byte loops only, and offsets are compared as integers before any
 * pointer arithmetic so a hostile length can never form an out-of-range pointer. */
#include "acpi-tables.h"

#define ACPI_LOW_MEM_END 0x100000ull

uint16_t acpiRd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t acpiRd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t acpiRd64(const uint8_t *p) {
    return (uint64_t)acpiRd32(p) | ((uint64_t)acpiRd32(p + 4) << 32);
}

uint8_t acpiChecksum(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum;
}

static void zeroBytes(void *p, size_t n) {
    volatile uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static bool sigEq(const uint8_t *t, const char *s) {
    return t[0] == (uint8_t)s[0] && t[1] == (uint8_t)s[1] && t[2] == (uint8_t)s[2] &&
           t[3] == (uint8_t)s[3];
}

/* ---- loader ------------------------------------------------------------------------------ */

static void copySig(char dst[4], const uint8_t *src) {
    for (int i = 0; i < 4; i++) {
        dst[i] = (char)src[i];
    }
}

static void recordReject(AcpiTableSet *set, uint64_t phys, const uint8_t *sig, Status st) {
    if (set->rejected < ACPI_MAX_REJECTS) {
        AcpiReject *r = &set->rejects[set->rejected];
        r->phys = phys;
        r->status = st;
        for (int i = 0; i < 4; i++) {
            r->signature[i] = sig != NULL ? (char)sig[i] : '?';
        }
    }
    set->rejected++;
}

/* Reads, validates and copies the table at `phys` into `*out` (not yet recorded in the set).
 * `sigOut` always gets a value: the header's signature, or "????" if the header was unreadable. */
static Status loadOne(const AcpiPhysOps *ops, uint64_t phys, const char *expectSig,
                      uint8_t sigOut[4], AcpiTable *out) {
    for (int i = 0; i < 4; i++) {
        sigOut[i] = '?';
    }
    uint8_t hdr[ACPI_TABLE_HEADER_LEN];
    Status st = ops->readPhys(ops->ctx, phys, hdr, ACPI_TABLE_HEADER_LEN);
    if (st == STATUS_ERR_NO_MEMORY) {
        return st;
    }
    if (st != STATUS_OK) {
        return STATUS_ERR_INVALID;
    }
    for (int i = 0; i < 4; i++) {
        sigOut[i] = hdr[i];
    }
    uint32_t len = acpiRd32(hdr + 4);
    if (len < ACPI_TABLE_HEADER_LEN || len > ACPI_TABLE_MAX_LEN || phys + len < phys) {
        return STATUS_ERR_INVALID;
    }
    if (expectSig != NULL && !sigEq(hdr, expectSig)) {
        return STATUS_ERR_INVALID;
    }
    uint8_t *buf = ops->alloc(ops->ctx, len);
    if (buf == NULL) {
        return STATUS_ERR_NO_MEMORY;
    }
    st = ops->readPhys(ops->ctx, phys, buf, len);
    if (st != STATUS_OK) {
        ops->free(ops->ctx, buf, len);
        return st == STATUS_ERR_NO_MEMORY ? st : STATUS_ERR_INVALID;
    }
    /* The second read is what we keep: re-verify it still matches the first (TOCTOU). */
    bool sameHdr = acpiRd32(buf + 4) == len;
    for (int i = 0; i < 4; i++) {
        sameHdr = sameHdr && buf[i] == hdr[i];
    }
    if (!sameHdr || acpiChecksum(buf, len) != 0) {
        ops->free(ops->ctx, buf, len);
        return STATUS_ERR_INVALID;
    }
    copySig(out->signature, buf);
    out->revision = buf[8];
    out->length = len;
    out->phys = phys;
    out->data = buf;
    return STATUS_OK;
}

void acpiTablesFree(const AcpiPhysOps *ops, AcpiTableSet *set) {
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->tables[i].data != NULL) {
            ops->free(ops->ctx, (void *)(uintptr_t)set->tables[i].data, set->tables[i].length);
            set->tables[i].data = NULL;
        }
    }
    set->count = 0;
}

static void resetSet(AcpiTableSet *s) {
    zeroBytes(s, sizeof(*s));
    s->fadtIndex = -1;
    s->dsdtIndex = -1;
}

static Status failLoad(const AcpiPhysOps *ops, AcpiTableSet *set, Status st) {
    acpiTablesFree(ops, set);
    set->fadtIndex = set->dsdtIndex = -1;
    return st;
}

static bool phys64Loaded(const AcpiTableSet *s, uint64_t phys) {
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->tables[i].phys == phys) {
            return true;
        }
    }
    return false;
}

Status acpiTablesLoad(const AcpiPhysOps *ops, uint64_t rsdpPhys, AcpiTableSet *out) {
    resetSet(out);
    if (rsdpPhys == 0) {
        return STATUS_ERR_NOT_FOUND;
    }

    /* The v1 part: 20 bytes, "RSD PTR ", checksum over those 20. */
    uint8_t buf[ACPI_RSDP_MAX_LEN];
    Status st = ops->readPhys(ops->ctx, rsdpPhys, buf, 20);
    if (st == STATUS_ERR_NO_MEMORY) {
        return st;
    }
    static const char rsdpSig[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    bool sigOk = st == STATUS_OK;
    for (int i = 0; sigOk && i < 8; i++) {
        sigOk = buf[i] == (uint8_t)rsdpSig[i];
    }
    if (!sigOk || acpiChecksum(buf, 20) != 0) {
        return STATUS_ERR_INVALID;
    }
    out->rsdpPhys = rsdpPhys;
    out->rsdpRevision = buf[15];
    out->rsdpLength = 20;
    for (int i = 0; i < 6; i++) {
        out->oemId[i] = (char)buf[9 + i];
    }
    out->rsdtPhys = acpiRd32(buf + 16);
    for (int i = 0; i < 20; i++) {
        out->rsdpRaw[i] = buf[i];
    }

    /* The v2 extension (revision >= 2): only then is it legal to read past 20 bytes. */
    if (out->rsdpRevision >= 2) {
        st = ops->readPhys(ops->ctx, rsdpPhys, buf, 36);
        if (st == STATUS_ERR_NO_MEMORY) {
            return failLoad(ops, out, st);
        }
        bool v2Ok = false;
        if (st == STATUS_OK) {
            uint32_t extLen = acpiRd32(buf + 20);
            if (extLen >= 36 && extLen <= ACPI_RSDP_MAX_LEN) {
                st = ops->readPhys(ops->ctx, rsdpPhys, buf, extLen);
                if (st == STATUS_ERR_NO_MEMORY) {
                    return failLoad(ops, out, st);
                }
                if (st == STATUS_OK && acpiChecksum(buf, extLen) == 0) {
                    v2Ok = true;
                    out->xsdtPhys = acpiRd64(buf + 24);
                    out->rsdpLength = extLen;
                    for (uint32_t i = 0; i < extLen; i++) {
                        out->rsdpRaw[i] = buf[i];
                    }
                }
            }
        }
        if (!v2Ok) {
            out->warnings |= ACPI_WARN_RSDP_V2_BAD;
        }
    }

    /* Root: the XSDT if there is one and it loads, else the RSDT. */
    AcpiTable root;
    uint8_t sig[4];
    bool haveRoot = false;
    if (out->xsdtPhys != 0) {
        st = loadOne(ops, out->xsdtPhys, "XSDT", sig, &root);
        if (st == STATUS_ERR_NO_MEMORY) {
            return failLoad(ops, out, st);
        }
        haveRoot = st == STATUS_OK;
        out->usedXsdt = haveRoot;
    }
    if (!haveRoot && out->rsdtPhys != 0) {
        st = loadOne(ops, out->rsdtPhys, "RSDT", sig, &root);
        if (st == STATUS_ERR_NO_MEMORY) {
            return failLoad(ops, out, st);
        }
        haveRoot = st == STATUS_OK;
        if (haveRoot && out->xsdtPhys != 0) {
            out->warnings |= ACPI_WARN_XSDT_FALLBACK;
        }
    }
    if (!haveRoot) {
        return failLoad(ops, out, STATUS_ERR_INVALID);
    }
    out->tables[out->count++] = root;

    /* Root entries, in order. */
    uint32_t entrySize = out->usedXsdt ? 8u : 4u;
    uint32_t entries = (root.length - ACPI_TABLE_HEADER_LEN) / entrySize;
    if ((root.length - ACPI_TABLE_HEADER_LEN) % entrySize != 0) {
        out->warnings |= ACPI_WARN_ROOT_TRAILING;
    }
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = root.data + ACPI_TABLE_HEADER_LEN + (size_t)i * entrySize;
        uint64_t phys = out->usedXsdt ? acpiRd64(e) : acpiRd32(e);
        if (phys == 0) {
            continue;
        }
        if (phys64Loaded(out, phys)) {
            out->warnings |= ACPI_WARN_DUPLICATE_ENTRY;
            continue;
        }
        if (out->count >= ACPI_MAX_TABLES - 1) { /* the last slot is the DSDT's */
            out->dropped++;
            out->warnings |= ACPI_WARN_TABLES_DROPPED;
            continue;
        }
        AcpiTable t;
        st = loadOne(ops, phys, NULL, sig, &t);
        if (st == STATUS_ERR_NO_MEMORY) {
            return failLoad(ops, out, st);
        }
        if (st != STATUS_OK) {
            recordReject(out, phys, sig, st);
            continue;
        }
        out->tables[out->count++] = t;
    }

    /* The FADT's DSDT. */
    for (uint32_t i = 0; i < out->count; i++) {
        if (sigEq((const uint8_t *)out->tables[i].signature, "FACP")) {
            out->fadtIndex = (int32_t)i;
            break;
        }
    }
    if (out->fadtIndex >= 0) {
        AcpiFadtInfo fadt;
        const AcpiTable *ft = &out->tables[out->fadtIndex];
        if (acpiParseFadt(ft->data, ft->length, &fadt) == STATUS_OK && fadt.dsdtPhys != 0) {
            if (fadt.dsdtMismatch) {
                out->warnings |= ACPI_WARN_DSDT_MISMATCH;
            }
            for (uint32_t i = 0; i < out->count; i++) {
                if (out->tables[i].phys == fadt.dsdtPhys &&
                    sigEq((const uint8_t *)out->tables[i].signature, "DSDT")) {
                    out->dsdtIndex = (int32_t)i;
                }
            }
            if (out->dsdtIndex < 0) {
                AcpiTable t;
                st = loadOne(ops, fadt.dsdtPhys, "DSDT", sig, &t);
                if (st == STATUS_ERR_NO_MEMORY) {
                    return failLoad(ops, out, st);
                }
                if (st == STATUS_OK) {
                    out->dsdtIndex = (int32_t)out->count;
                    out->tables[out->count++] = t;
                } else {
                    recordReject(out, fadt.dsdtPhys, sig, st);
                }
            }
        }
    }
    if (out->dsdtIndex < 0) {
        out->warnings |= ACPI_WARN_NO_DSDT;
    }
    return STATUS_OK;
}

const AcpiTable *acpiTablesFind(const AcpiTableSet *s, const char sig[4], uint32_t instance) {
    for (uint32_t i = 0; i < s->count; i++) {
        const AcpiTable *t = &s->tables[i];
        if (t->signature[0] == sig[0] && t->signature[1] == sig[1] && t->signature[2] == sig[2] &&
            t->signature[3] == sig[3]) {
            if (instance == 0) {
                return t;
            }
            instance--;
        }
    }
    return NULL;
}

/* ---- parsers ----------------------------------------------------------------------------- */

/* Common prologue: zero `out`, check signature, minimum length and the header's Length field. */
static Status checkHeader(const uint8_t *t, uint32_t len, const char *sig, uint32_t minLen) {
    if (len < ACPI_TABLE_HEADER_LEN || len < minLen || !sigEq(t, sig) || acpiRd32(t + 4) != len) {
        return STATUS_ERR_INVALID;
    }
    return STATUS_OK;
}

static AcpiGas readGas(const uint8_t *p) {
    AcpiGas g;
    g.spaceId = p[0];
    g.bitWidth = p[1];
    g.bitOffset = p[2];
    g.accessSize = p[3];
    g.address = acpiRd64(p + 4);
    return g;
}

/* One FADT register block: the X_ GAS when it is present (table long enough) and nonzero, else the
 * legacy u32 address synthesized as SystemIO of `legacyLen` bytes, else absent (ACPI 6.5 §5.2.9).
 * `*usedX` reports which. */
static AcpiGas fadtBlock(const uint8_t *t, uint32_t len, uint32_t legacyOff, uint32_t xOff,
                         uint8_t legacyLen, bool *usedX) {
    AcpiGas g;
    zeroBytes(&g, sizeof(g));
    *usedX = false;
    if ((uint64_t)xOff + 12 <= len) {
        AcpiGas x = readGas(t + xOff);
        if (x.address != 0) {
            *usedX = true;
            return x;
        }
    }
    uint32_t legacy = acpiRd32(t + legacyOff);
    if (legacy != 0) {
        g.spaceId = 1;
        g.bitWidth = (uint8_t)(legacyLen > 31 ? 255 : legacyLen * 8);
        g.address = legacy;
    }
    return g;
}

Status acpiParseFadt(const uint8_t *t, uint32_t len, AcpiFadtInfo *out) {
    zeroBytes(out, sizeof(*out));
    if (checkHeader(t, len, "FACP", 116) != STATUS_OK) {
        return STATUS_ERR_INVALID;
    }
    out->revision = t[8];
    out->flags = acpiRd32(t + 112);
    out->sciInt = acpiRd16(t + 46);
    out->smiCmd = acpiRd32(t + 48);
    out->acpiEnable = t[52];
    out->acpiDisable = t[53];
    out->pm1EvtLen = t[88];
    out->pm1CntLen = t[89];
    out->pm2CntLen = t[90];
    out->pmTmrLen = t[91];
    out->gpe0Len = t[92];
    out->gpe1Len = t[93];
    out->gpe1Base = t[94];
    out->century = t[108];
    out->iapcBootArch = acpiRd16(t + 109);
    out->hwReduced = ((out->flags >> 20) & 1) != 0;
    out->pmTimer32Bit = ((out->flags >> 8) & 1) != 0;

    bool x;
    bool xTmr;
    out->pm1aEvt = fadtBlock(t, len, 56, 148, out->pm1EvtLen, &x);
    out->pm1bEvt = fadtBlock(t, len, 60, 160, out->pm1EvtLen, &x);
    out->pm1aCnt = fadtBlock(t, len, 64, 172, out->pm1CntLen, &x);
    out->pm1bCnt = fadtBlock(t, len, 68, 184, out->pm1CntLen, &x);
    out->pm2Cnt = fadtBlock(t, len, 72, 196, out->pm2CntLen, &x);
    out->pmTmr = fadtBlock(t, len, 76, 208, out->pmTmrLen, &xTmr);
    out->gpe0 = fadtBlock(t, len, 80, 220, out->gpe0Len, &x);
    out->gpe1 = fadtBlock(t, len, 84, 232, out->gpe1Len, &x);
    out->pmTimerPresent = !out->hwReduced && out->pmTmr.address != 0 &&
                          (out->pmTmr.spaceId == 0 || out->pmTmr.spaceId == 1) &&
                          (xTmr || out->pmTmrLen == 4);

    if (len >= 129) {
        out->resetReg = readGas(t + 116);
        out->resetValue = t[128];
        out->resetSupported = ((out->flags >> 10) & 1) != 0 && out->resetReg.address != 0 &&
                              out->resetReg.spaceId <= 2;
    }
    if (len >= 132) {
        out->minorVersion = t[131];
    }
    if (len >= 268) {
        out->sleepControl = readGas(t + 244);
        out->sleepStatus = readGas(t + 256);
    }

    /* DSDT and FACS: the X_ field wins when nonzero (and the table is long enough). */
    uint64_t dsdt = acpiRd32(t + 40);
    uint64_t facs = acpiRd32(t + 36);
    if (len >= 148) {
        uint64_t xFacs = acpiRd64(t + 132);
        uint64_t xDsdt = acpiRd64(t + 140);
        if (xDsdt != 0) {
            out->dsdtMismatch = dsdt != 0 && dsdt != xDsdt;
            dsdt = xDsdt;
        }
        if (xFacs != 0) {
            facs = xFacs;
        }
    }
    out->dsdtPhys = dsdt;
    out->facsPhys = facs;
    return STATUS_OK;
}

static bool madtHasCpu(const AcpiMadtInfo *m, uint32_t apicId) {
    for (uint32_t i = 0; i < m->cpuCount; i++) {
        if (m->cpus[i].apicId == apicId) {
            return true;
        }
    }
    return false;
}

/* Bit 1 of a CPU entry's flags is Online-Capable only from MADT revision 5 (ACPI 6.3); before that
 * it is reserved and real firmware has left junk in it (D-170), so older tables use bit 0 alone. */
static void madtAddCpu(AcpiMadtInfo *m, uint8_t madtRev, uint32_t apicId, uint32_t uid,
                       uint32_t flags, bool x2apic) {
    uint32_t usableMask = madtRev >= 5 ? 3u : 1u;
    if ((flags & usableMask) == 0) {
        m->cpusDisabled++;
        return;
    }
    if (madtHasCpu(m, apicId)) {
        return;
    }
    if (m->cpuCount >= ACPI_MAX_CPUS) {
        m->cpusDropped++;
        return;
    }
    AcpiCpu *c = &m->cpus[m->cpuCount++];
    c->apicId = apicId;
    c->uid = uid;
    c->flags = flags;
    c->x2apic = x2apic;
}

static void madtAddLapicNmi(AcpiMadtInfo *m, uint32_t uid, uint16_t flags, uint8_t lint) {
    if (m->lapicNmiCount >= ACPI_MAX_LAPIC_NMIS) {
        m->droppedEntries++;
        return;
    }
    AcpiLapicNmi *n = &m->lapicNmis[m->lapicNmiCount++];
    n->uid = uid;
    n->flags = flags;
    n->lint = lint;
}

Status acpiParseMadt(const uint8_t *t, uint32_t len, AcpiMadtInfo *out) {
    zeroBytes(out, sizeof(*out));
    if (checkHeader(t, len, "APIC", 44) != STATUS_OK) {
        return STATUS_ERR_INVALID;
    }
    out->lapicAddress = acpiRd32(t + 36);
    out->flags = acpiRd32(t + 40);
    out->pcatCompat = (out->flags & 1) != 0;

    uint32_t off = 44;
    while ((uint64_t)off + 2 <= len) {
        uint8_t type = t[off];
        uint32_t elen = t[off + 1];
        if (elen < 2 || (uint64_t)off + elen > len) {
            zeroBytes(out, sizeof(*out));
            return STATUS_ERR_INVALID; /* the infinite-loop and overrun guard */
        }
        const uint8_t *e = t + off;
        switch (type) {
            case 0: /* processor local APIC */
                if (elen < 8) {
                    out->malformedEntries++;
                } else if (e[3] == 0xFF) {
                    out->malformedEntries++; /* 0xFF is the xAPIC broadcast ID */
                } else {
                    madtAddCpu(out, t[8], e[3], e[2], acpiRd32(e + 4), false);
                }
                break;
            case 1: /* I/O APIC */
                if (elen < 12) {
                    out->malformedEntries++;
                } else if (out->ioapicCount >= ACPI_MAX_IOAPICS) {
                    out->droppedEntries++;
                } else {
                    AcpiIoApic *io = &out->ioapics[out->ioapicCount++];
                    io->id = e[2];
                    io->address = acpiRd32(e + 4);
                    io->gsiBase = acpiRd32(e + 8);
                }
                break;
            case 2: /* interrupt source override */
                if (elen < 10) {
                    out->malformedEntries++;
                } else if (out->isoCount >= ACPI_MAX_ISOS) {
                    out->droppedEntries++;
                } else {
                    AcpiIso *iso = &out->isos[out->isoCount++];
                    iso->bus = e[2];
                    iso->source = e[3];
                    iso->gsi = acpiRd32(e + 4);
                    iso->flags = acpiRd16(e + 8);
                }
                break;
            case 3: /* NMI source */
                if (elen < 8) {
                    out->malformedEntries++;
                } else if (out->nmiSourceCount >= ACPI_MAX_NMI_SOURCES) {
                    out->droppedEntries++;
                } else {
                    AcpiNmiSource *n = &out->nmiSources[out->nmiSourceCount++];
                    n->flags = acpiRd16(e + 2);
                    n->gsi = acpiRd32(e + 4);
                }
                break;
            case 4: /* local APIC NMI */
                if (elen < 6) {
                    out->malformedEntries++;
                } else {
                    uint32_t uid = e[2] == 0xFF ? ACPI_LAPIC_NMI_ALL : e[2];
                    madtAddLapicNmi(out, uid, acpiRd16(e + 3), e[5]);
                }
                break;
            case 5: /* local APIC address override */
                if (elen < 12) {
                    out->malformedEntries++;
                } else {
                    out->lapicAddress = acpiRd64(e + 4);
                }
                break;
            case 9: /* processor local x2APIC */
                if (elen < 16) {
                    out->malformedEntries++;
                } else {
                    if (acpiRd32(e + 4) == 0xFFFFFFFFu) {
                        out->malformedEntries++; /* the x2APIC broadcast ID */
                    } else {
                        madtAddCpu(out, t[8], acpiRd32(e + 4), acpiRd32(e + 12), acpiRd32(e + 8),
                                   true);
                    }
                }
                break;
            case 0xA: /* local x2APIC NMI */
                if (elen < 12) {
                    out->malformedEntries++;
                } else {
                    madtAddLapicNmi(out, acpiRd32(e + 4), acpiRd16(e + 2), e[8]);
                }
                break;
            default:
                break; /* unknown types are skipped by their own length */
        }
        off += elen;
    }
    return STATUS_OK;
}

Status acpiParseMcfg(const uint8_t *t, uint32_t len, AcpiMcfgInfo *out) {
    zeroBytes(out, sizeof(*out));
    if (checkHeader(t, len, "MCFG", 44) != STATUS_OK) {
        return STATUS_ERR_INVALID;
    }
    uint32_t body = len - 44;
    out->trailing = (body % 16) != 0;
    for (uint32_t i = 0; i < body / 16; i++) {
        const uint8_t *e = t + 44 + (size_t)i * 16;
        uint64_t base = acpiRd64(e);
        if (base == 0 || e[10] > e[11]) {
            out->malformed++;
        } else if (out->count >= ACPI_MAX_MCFG) {
            out->dropped++;
        } else {
            AcpiMcfgSeg *s = &out->segs[out->count++];
            s->base = base;
            s->segment = acpiRd16(e + 8);
            s->startBus = e[10];
            s->endBus = e[11];
        }
    }
    return STATUS_OK;
}

Status acpiParseHpet(const uint8_t *t, uint32_t len, AcpiHpetInfo *out) {
    zeroBytes(out, sizeof(*out));
    if (checkHeader(t, len, "HPET", 56) != STATUS_OK) {
        return STATUS_ERR_INVALID;
    }
    if (t[40] != 0) { /* the base address must be in system memory space */
        return STATUS_ERR_INVALID;
    }
    out->blockId = acpiRd32(t + 36);
    out->base = acpiRd64(t + 44);
    out->number = t[52];
    out->minTick = acpiRd16(t + 53);
    out->pageProtection = t[55];
    out->comparators = (uint8_t)(((out->blockId >> 8) & 0x1F) + 1);
    out->counter64 = ((out->blockId >> 13) & 1) != 0;
    return STATUS_OK;
}

Status acpiParseIvrs(const uint8_t *t, uint32_t len, AcpiIvrsInfo *out) {
    zeroBytes(out, sizeof(*out));
    if (checkHeader(t, len, "IVRS", 48) != STATUS_OK) {
        return STATUS_ERR_INVALID;
    }
    out->ivInfo = acpiRd32(t + 36);
    out->raw = t;
    out->rawLen = len;
    uint32_t off = 48;
    while ((uint64_t)off + 4 <= len) {
        uint8_t type = t[off];
        uint32_t blen = acpiRd16(t + off + 2);
        if (blen < 4 || (uint64_t)off + blen > len) {
            zeroBytes(out, sizeof(*out));
            return STATUS_ERR_INVALID;
        }
        const uint8_t *b = t + off;
        if (type == 0x10 || type == 0x11 || type == 0x40) {
            uint32_t minLen = type == 0x10 ? 24 : 40;
            if (blen < minLen) {
                out->malformed++;
            } else if (out->ivhdCount >= ACPI_MAX_IVHD) {
                out->dropped++;
            } else {
                AcpiIvhd *h = &out->ivhd[out->ivhdCount++];
                h->type = type;
                h->flags = b[1];
                h->deviceId = acpiRd16(b + 4);
                h->capOffset = acpiRd16(b + 6);
                h->base = acpiRd64(b + 8);
                h->segment = acpiRd16(b + 16);
                h->info = acpiRd16(b + 18);
                h->featOrAttr = acpiRd32(b + 20);
                h->efr = type == 0x10 ? 0 : acpiRd64(b + 24);
            }
        } else if (type >= 0x20 && type <= 0x22) {
            out->ivmdCount++;
        }
        off += blen;
    }
    return STATUS_OK;
}

void acpiParseAll(const AcpiTableSet *s, AcpiInfo *out) {
    zeroBytes(out, sizeof(*out));
    const AcpiTable *t;
    t = acpiTablesFind(s, "FACP", 0);
    out->fadtStatus =
        t != NULL ? acpiParseFadt(t->data, t->length, &out->fadt) : STATUS_ERR_NOT_FOUND;
    t = acpiTablesFind(s, "APIC", 0);
    out->madtStatus =
        t != NULL ? acpiParseMadt(t->data, t->length, &out->madt) : STATUS_ERR_NOT_FOUND;
    t = acpiTablesFind(s, "MCFG", 0);
    out->mcfgStatus =
        t != NULL ? acpiParseMcfg(t->data, t->length, &out->mcfg) : STATUS_ERR_NOT_FOUND;
    t = acpiTablesFind(s, "HPET", 0);
    out->hpetStatus =
        t != NULL ? acpiParseHpet(t->data, t->length, &out->hpet) : STATUS_ERR_NOT_FOUND;
    t = acpiTablesFind(s, "IVRS", 0);
    out->ivrsStatus =
        t != NULL ? acpiParseIvrs(t->data, t->length, &out->ivrs) : STATUS_ERR_NOT_FOUND;
    while (acpiTablesFind(s, "HPET", out->hpetTableCount) != NULL) {
        out->hpetTableCount++;
    }
}

/* ---- physical range policy --------------------------------------------------------------- */

static bool typeAllowed(uint32_t type) {
    return type == BOOT_MEM_USABLE || type == BOOT_MEM_RESERVED || type == BOOT_MEM_ACPI_RECLAIM ||
           type == BOOT_MEM_ACPI_NVS;
}

bool acpiPhysRangeAllowed(const BootMemRegion *map, uint32_t n, uint64_t phys, uint64_t len) {
    if (len == 0 || phys + len < phys) {
        return false;
    }
    uint64_t end = phys + len;
    uint64_t cur = phys;
    while (cur < end) {
        if (cur < ACPI_LOW_MEM_END) {
            cur = end < ACPI_LOW_MEM_END ? end : ACPI_LOW_MEM_END;
            continue;
        }
        const BootMemRegion *hit = NULL;
        for (uint32_t i = 0; i < n; i++) {
            if (cur >= map[i].base && cur - map[i].base < map[i].length) {
                hit = &map[i];
                break;
            }
        }
        if (hit == NULL || !typeAllowed(hit->type)) {
            return false;
        }
        uint64_t regionEnd = hit->base + hit->length;
        if (regionEnd <= cur) {
            return false; /* base+length wrapped (an unvalidated map): never move the cursor back */
        }
        cur = end < regionEnd ? end : regionEnd;
    }
    return true;
}
