#include "disk.h"

#include "bootmem.h"
#include "rm.h"

#include <stdbool.h>

/* Fixed low-memory scratch offsets (docs/specs/bios-boot.md §3, D-101). */
#define DISK_DAP_ADDR        0x1000u
#define DISK_EDD_PARAMS_ADDR 0x1180u
#define DISK_BOUNCE_ADDR     0x2000u

#define DISK_SECTOR_SIZE    512u
#define DISK_BOUNCE_SECTORS 32u /* 16 KiB bounce buffer / 512-byte sectors */
#define DISK_MAX_ATTEMPTS   3u

/* entry.asm captures DL (the BIOS boot drive number, docs/specs/bios-boot.md §2 step 9) into this
 * byte as the very first thing it does, before anything else can clobber it. */
extern uint8_t bootDrive;

static BootStatus diskCheckEdd(uint8_t drive) {
    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    r.eax = 0x4100u; /* AH=41h: check extensions present */
    r.ebx = 0x55AAu;
    r.edx = drive;
    rmInt(0x13, &r);
    if ((r.eflags & 1u) != 0) {
        return BOOT_ERR_IO;
    }
    if ((r.ebx & 0xFFFFu) != 0xAA55u || (r.ecx & 1u) == 0) {
        return BOOT_ERR_IO;
    }
    return BOOT_OK;
}

static BootStatus diskGetSectorCount(uint8_t drive, uint64_t *outSectorCount) {
    uint8_t *buf = (uint8_t *)(uintptr_t)DISK_EDD_PARAMS_ADDR;
    bootMemset(buf, 0, 0x1Eu);
    uint16_t bufSize = 0x1Eu; /* the "result buffer size" field the BIOS reads before filling it */
    bootMemcpy(buf, &bufSize, sizeof(bufSize));

    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    r.eax = 0x4800u; /* AH=48h: get drive parameters (extended) */
    r.edx = drive;
    r.esi = DISK_EDD_PARAMS_ADDR;
    r.ds = 0;
    rmInt(0x13, &r);
    if ((r.eflags & 1u) != 0) {
        return BOOT_ERR_IO;
    }

    uint64_t totalSectors = 0;
    bootMemcpy(&totalSectors, buf + 0x10, sizeof(totalSectors)); /* offset 0x10: qword LBA count */
    *outSectorCount = totalSectors;
    return BOOT_OK;
}

/* One chunk (<= DISK_BOUNCE_SECTORS sectors) into the fixed bounce buffer, retried up to
 * DISK_MAX_ATTEMPTS times with a disk-system reset (AH=00h) between failures -- the same retry
 * policy stage1 uses for its own reads (docs/specs/bios-boot.md §2 step 6), for the same reason:
 * a transient BIOS/controller error on the first try is common enough on real hardware to be
 * worth one retry cycle rather than a hard failure. The 16-byte Device Address Packet (EDD,
 * docs/specs/bios-boot.md's DAP layout) is rebuilt from scratch on every attempt, since some
 * BIOSes overwrite the count field with the actual transferred count. */
static BootStatus diskReadChunk(uint8_t drive, uint64_t lba, uint32_t count) {
    uint8_t *dap = (uint8_t *)(uintptr_t)DISK_DAP_ADDR;
    uint16_t countU16 = (uint16_t)count;
    uint16_t bufOff = 0;
    uint16_t bufSeg = (uint16_t)(DISK_BOUNCE_ADDR >> 4);

    for (uint32_t attempt = 0; attempt < DISK_MAX_ATTEMPTS; attempt++) {
        bootMemset(dap, 0, 16);
        dap[0] = 0x10;
        dap[1] = 0;
        bootMemcpy(dap + 2, &countU16, sizeof(countU16));
        bootMemcpy(dap + 4, &bufOff, sizeof(bufOff));
        bootMemcpy(dap + 6, &bufSeg, sizeof(bufSeg));
        bootMemcpy(dap + 8, &lba, sizeof(lba));

        RmRegs r;
        bootMemset(&r, 0, sizeof(r));
        r.eax = 0x4200u; /* AH=42h: extended read, AL=0 (ignored) */
        r.edx = drive;
        r.esi = DISK_DAP_ADDR; /* DS:SI -> the DAP */
        r.ds = 0;
        rmInt(0x13, &r);
        if ((r.eflags & 1u) == 0) {
            return BOOT_OK;
        }

        RmRegs reset;
        bootMemset(&reset, 0, sizeof(reset));
        reset.edx = drive; /* AH=00h: disk system reset */
        rmInt(0x13, &reset);
    }
    return BOOT_ERR_IO;
}

static BootStatus diskRead(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    uint8_t *out = (uint8_t *)dst;
    while (count > 0) {
        uint32_t chunk = count < DISK_BOUNCE_SECTORS ? count : DISK_BOUNCE_SECTORS;
        BootStatus st = diskReadChunk(bootDrive, lba, chunk);
        if (st != BOOT_OK) {
            return st;
        }
        bootMemcpy(out, (const void *)(uintptr_t)DISK_BOUNCE_ADDR, chunk * DISK_SECTOR_SIZE);
        lba += chunk;
        count -= chunk;
        out += chunk * DISK_SECTOR_SIZE;
    }
    return BOOT_OK;
}

BootStatus diskProbe(BootBlockDev *dev) {
    BootStatus st = diskCheckEdd(bootDrive);
    if (st != BOOT_OK) {
        return st;
    }
    uint64_t sectorCount = 0;
    st = diskGetSectorCount(bootDrive, &sectorCount);
    if (st != BOOT_OK) {
        return st;
    }
    dev->read = diskRead;
    dev->ctx = NULL;
    dev->sectorSize = DISK_SECTOR_SIZE;
    dev->sectorCount = sectorCount;
    return BOOT_OK;
}
