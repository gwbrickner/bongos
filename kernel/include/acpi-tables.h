/* Pure ACPI table loader and parsers (ARCHITECTURE §14, D-166/D-167, ROADMAP M3.1). Nothing here
 * touches hardware, klog, panic or an arch: firmware memory is only ever read through the
 * AcpiPhysOps callback, and every accepted table is copied into memory the callback's `alloc`
 * returns. Host-tested (tests/host/kernel_acpi_test.c) against synthetic and stored QEMU tables.
 * All multi-byte fields are read byte-wise little-endian: table bytes are only 1/4-byte aligned,
 * so a struct cast would trip UBSan's alignment check. */
#ifndef KERNEL_ACPI_TABLES_H
#define KERNEL_ACPI_TABLES_H

#include "bootinfo.h"
#include "uapi/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACPI_TABLE_HEADER_LEN 36u
#define ACPI_TABLE_MAX_LEN    0x400000u /* 4 MiB: anything larger is rejected */
#define ACPI_RSDP_MAX_LEN     256u
#define ACPI_MAX_TABLES       128u /* root + entries + DSDT */
#define ACPI_MAX_REJECTS      16u
#define ACPI_MAX_CPUS         256u
#define ACPI_MAX_IOAPICS      16u
#define ACPI_MAX_ISOS         32u
#define ACPI_MAX_NMI_SOURCES  16u
#define ACPI_MAX_LAPIC_NMIS   ACPI_MAX_CPUS
#define ACPI_MAX_MCFG         16u
#define ACPI_MAX_IVHD         8u

#define ACPI_LAPIC_NMI_ALL 0xFFFFFFFFu

/* How the loader reaches the world. `readPhys` copies [phys, phys+len) into `dst`: INVALID if the
 * range is unreadable or disallowed, NO_MEMORY if no mapping could be made (the loader aborts on
 * NO_MEMORY, and treats any other error as "this table is unreadable"). `alloc` returns NULL on
 * OOM; `free` receives the same length `alloc` was given. */
typedef struct {
    void *ctx;
    Status (*readPhys)(void *ctx, uint64_t phys, void *dst, uint32_t len);
    void *(*alloc)(void *ctx, uint32_t len);
    void (*free)(void *ctx, void *p, uint32_t len);
} AcpiPhysOps;

typedef struct {
    char signature[4]; /* raw, not NUL-terminated */
    uint8_t revision;
    uint32_t length;
    uint64_t phys;       /* the firmware location: an identity only, never dereference */
    const uint8_t *data; /* owned copy, checksum verified, `length` bytes */
} AcpiTable;

typedef struct {
    uint64_t phys;
    char signature[4];
    Status status;
} AcpiReject;

#define ACPI_WARN_RSDP_V2_BAD     (1u << 0) /* v2 extended checksum/length bad: v1 fields used */
#define ACPI_WARN_XSDT_FALLBACK   (1u << 1)
#define ACPI_WARN_ROOT_TRAILING   (1u << 2) /* (len-36) % entrySize != 0: floored */
#define ACPI_WARN_TABLES_DROPPED  (1u << 3)
#define ACPI_WARN_DSDT_MISMATCH   (1u << 4) /* X_DSDT and DSDT both set and differ: X_DSDT used */
#define ACPI_WARN_NO_DSDT         (1u << 5)
#define ACPI_WARN_DUPLICATE_ENTRY (1u << 6)

typedef struct {
    uint64_t rsdpPhys;
    uint8_t rsdpRevision;
    uint32_t rsdpLength; /* 20, or the v2 Length field */
    uint8_t rsdpRaw[ACPI_RSDP_MAX_LEN];
    char oemId[6];
    uint64_t rsdtPhys, xsdtPhys;
    bool usedXsdt;
    AcpiTable tables[ACPI_MAX_TABLES]; /* [0] = root, then entries in root order, then the DSDT */
    uint32_t count;
    int32_t fadtIndex, dsdtIndex; /* -1 = none */
    AcpiReject rejects[ACPI_MAX_REJECTS];
    uint32_t rejected; /* counts every rejection; rejects[] holds the first ACPI_MAX_REJECTS */
    uint32_t dropped;
    uint32_t warnings;
} AcpiTableSet;

typedef struct {
    uint8_t spaceId, bitWidth, bitOffset, accessSize;
    uint64_t address; /* 0 = absent */
} AcpiGas;

typedef struct {
    uint8_t revision, minorVersion;
    uint32_t flags;
    uint16_t iapcBootArch;
    uint16_t sciInt;
    uint32_t smiCmd;
    uint8_t acpiEnable, acpiDisable;
    AcpiGas pm1aEvt, pm1bEvt, pm1aCnt, pm1bCnt, pm2Cnt, pmTmr, gpe0, gpe1;
    uint8_t pm1EvtLen, pm1CntLen, pm2CntLen, pmTmrLen, gpe0Len, gpe1Len, gpe1Base, century;
    bool pmTimerPresent, pmTimer32Bit, resetSupported, hwReduced, dsdtMismatch;
    AcpiGas resetReg;
    uint8_t resetValue;
    AcpiGas sleepControl, sleepStatus; /* revision 5+, else absent */
    uint64_t dsdtPhys, facsPhys;       /* the FACS is recorded by address only, never copied */
} AcpiFadtInfo;

typedef struct {
    uint32_t apicId, uid, flags;
    bool x2apic;
} AcpiCpu;
typedef struct {
    uint8_t id;
    uint32_t address, gsiBase;
} AcpiIoApic;
typedef struct {
    uint8_t bus, source;
    uint32_t gsi;
    uint16_t flags; /* MPS INTI flags: bits 0-1 polarity, 2-3 trigger */
} AcpiIso;
typedef struct {
    uint16_t flags;
    uint32_t gsi;
} AcpiNmiSource;
typedef struct {
    uint32_t uid; /* ACPI_LAPIC_NMI_ALL = every processor */
    uint16_t flags;
    uint8_t lint;
} AcpiLapicNmi;

typedef struct {
    uint64_t lapicAddress;
    uint32_t flags;
    bool pcatCompat;
    AcpiCpu cpus[ACPI_MAX_CPUS];
    uint32_t cpuCount, cpusDropped, cpusDisabled;
    AcpiIoApic ioapics[ACPI_MAX_IOAPICS];
    uint32_t ioapicCount;
    AcpiIso isos[ACPI_MAX_ISOS];
    uint32_t isoCount;
    AcpiNmiSource nmiSources[ACPI_MAX_NMI_SOURCES];
    uint32_t nmiSourceCount;
    AcpiLapicNmi lapicNmis[ACPI_MAX_LAPIC_NMIS];
    uint32_t lapicNmiCount;
    uint32_t droppedEntries, malformedEntries;
} AcpiMadtInfo;

typedef struct {
    uint64_t base;
    uint16_t segment;
    uint8_t startBus, endBus;
} AcpiMcfgSeg;
typedef struct {
    AcpiMcfgSeg segs[ACPI_MAX_MCFG];
    uint32_t count, dropped, malformed;
    bool trailing;
} AcpiMcfgInfo;

typedef struct {
    uint64_t base;
    uint32_t blockId;
    uint8_t number;
    uint16_t minTick;
    uint8_t comparators;
    bool counter64;
    uint8_t pageProtection;
} AcpiHpetInfo;

typedef struct {
    uint8_t type, flags;
    uint16_t deviceId, capOffset, segment, info;
    uint64_t base;
    uint32_t featOrAttr;
    uint64_t efr;
} AcpiIvhd;
typedef struct {
    uint32_t ivInfo;
    AcpiIvhd ivhd[ACPI_MAX_IVHD];
    uint32_t ivhdCount, ivmdCount, dropped, malformed;
    const uint8_t *raw; /* points into the kept copy of the table */
    uint32_t rawLen;
} AcpiIvrsInfo;

typedef struct {
    Status fadtStatus, madtStatus, mcfgStatus, hpetStatus, ivrsStatus; /* OK/NOT_FOUND/INVALID */
    uint32_t hpetTableCount;
    AcpiFadtInfo fadt;
    AcpiMadtInfo madt;
    AcpiMcfgInfo mcfg;
    AcpiHpetInfo hpet;
    AcpiIvrsInfo ivrs;
} AcpiInfo;

/* Byte sum mod 256 over `len` bytes: 0 for a valid table. Pure. */
uint8_t acpiChecksum(const uint8_t *p, uint32_t len);

/* Little-endian byte readers (no alignment requirement). Pure. */
uint16_t acpiRd16(const uint8_t *p);
uint32_t acpiRd32(const uint8_t *p);
uint64_t acpiRd64(const uint8_t *p);

/* Loads the RSDP at `rsdpPhys`, then the XSDT (RSDT fallback) and every table it lists, plus the
 * FADT's DSDT, copying each validated table through `ops`. OK if a root table was loaded (single
 * tables may still have been rejected or dropped: see `rejected`, `dropped`, `warnings`);
 * NOT_FOUND if `rsdpPhys` is 0; INVALID if there is no valid RSDP or root; NO_MEMORY on any
 * allocation (or readPhys NO_MEMORY) failure. On any non-OK return every allocation has been
 * freed and `out->count == 0`. No locks, pure, reentrant. */
Status acpiTablesLoad(const AcpiPhysOps *ops, uint64_t rsdpPhys, AcpiTableSet *out);

/* Frees every table copy `set` owns and zeroes `count`. No locks, pure. */
void acpiTablesFree(const AcpiPhysOps *ops, AcpiTableSet *set);

/* The `instance`-th table (0-based, in set order) whose signature is `sig`, or NULL. Pure. */
const AcpiTable *acpiTablesFind(const AcpiTableSet *s, const char sig[4], uint32_t instance);

/* The per-table parsers. Each zeroes `*out`, requires the right signature, `len` at least the
 * table's minimum and the header's Length field equal to `len` (else INVALID), and is safe for any
 * bytes given `len` readable bytes. They do not re-check the checksum (the loader does). */
Status acpiParseFadt(const uint8_t *t, uint32_t len, AcpiFadtInfo *out);
Status acpiParseMadt(const uint8_t *t, uint32_t len, AcpiMadtInfo *out);
Status acpiParseMcfg(const uint8_t *t, uint32_t len, AcpiMcfgInfo *out);
Status acpiParseHpet(const uint8_t *t, uint32_t len, AcpiHpetInfo *out);
Status acpiParseIvrs(const uint8_t *t, uint32_t len, AcpiIvrsInfo *out);

/* Parses the first FACP, APIC, MCFG, HPET and IVRS in `s` into `out` (status NOT_FOUND for a
 * missing table). `out->ivrs.raw` aliases the set's copy, so the set must outlive `out`. Pure. */
void acpiParseAll(const AcpiTableSet *s, AcpiInfo *out);

/* True if [phys, phys+len) may be read as firmware table memory: len != 0, no overflow, and every
 * byte lies below 1 MiB (EBDA/BIOS area, where E820 often has holes) or inside a `map` region of
 * type USABLE, RESERVED, ACPI_RECLAIM or ACPI_NVS. KERNEL/INITRD/LOADER_RECLAIM/BAD/FRAMEBUFFER
 * and holes at or above 1 MiB are refused. Pure. */
bool acpiPhysRangeAllowed(const BootMemRegion *map, uint32_t n, uint64_t phys, uint64_t len);

#endif
