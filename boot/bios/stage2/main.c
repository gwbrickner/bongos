/* stage2's C entry (D-103/D-104): the 16-bit entry (entry.asm) already switched to protected
 * mode before calling this. Uses the same shared serial/CPU drivers the UEFI loader does
 * (boot/common/hw/serial.c, cpu.c) -- proving the i386 build of the shared loader code, not just
 * a one-off toolchain smoke test. _Noreturn: pm_entry's caller treats returning here as
 * unreachable. */
#include "boot-status.h"
#include "bootheap.h"
#include "bootmem.h"
#include "e820.h"
#include "loader-cpu.h"
#include "loader-serial.h"
#include "memmap.h"
#include "rm.h"

#include <stdbool.h>

#define E820_MAX_REGIONS 64u

static void haltForever(void) {
    for (;;) {
        __asm__ volatile("cli\n\thlt");
    }
}

static void writeHexNibble(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    char c[2];
    c[0] = digits[v & 0xFu];
    c[1] = '\0';
    loaderSerialWriteString(c);
}

static void writeHex32(uint32_t v) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        writeHexNibble(v >> shift);
    }
}

static void writeHex64(uint64_t v) {
    writeHex32((uint32_t)(v >> 32));
    writeHex32((uint32_t)v);
}

static const char *memTypeName(uint32_t type) {
    switch (type) {
    case BOOT_MEM_USABLE:
        return "usable";
    case BOOT_MEM_RESERVED:
        return "reserved";
    case BOOT_MEM_ACPI_RECLAIM:
        return "acpi-reclaim";
    case BOOT_MEM_ACPI_NVS:
        return "acpi-nvs";
    case BOOT_MEM_BAD:
        return "bad";
    default:
        return "other";
    }
}

/* First real exercise of the thunk (D-102): INT 12h takes no input registers and returns the
 * conventional-memory size in KiB in AX -- about the simplest possible real BIOS call to prove
 * rmInt() actually works end to end (the far-call-through-the-IVT emulation, the PM<->RM<->PM
 * segment/stack juggling, and the register marshaling) before anything real (E820, disk, VBE)
 * depends on it. */
static void thunkSelfTest(void) {
    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    rmInt(0x12, &r);
    loaderSerialWriteString("loader: INT 12h reports ");
    loaderSerialWriteUint(r.eax & 0xFFFFu);
    loaderSerialWriteString(" KiB low memory\n");
}

/* Step 8 (D-107): scans E820, normalizes it, and builds the heap -- the second real thunk
 * exercise (after thunkSelfTest's INT 12h) and the first one whose output later steps (disk,
 * kernel load) actually depend on. Logs every normalized region and the resulting heap capacity
 * over serial so a QEMU run is enough to confirm it end to end (D-084-style: this gets exercised
 * under more than one QEMU memory size, from the Makefile/harness side, not by branching here). */
static void memMapSelfTest(void) {
    static MemMapInput e820Regions[E820_MAX_REGIONS];
    uint32_t nE820 = e820Scan(e820Regions, E820_MAX_REGIONS);
    loaderSerialWriteString("loader: E820 returned ");
    loaderSerialWriteUint(nE820);
    loaderSerialWriteString(" region(s)\n");

    static BootMemRegion normalized[E820_MAX_REGIONS];
    static uint64_t scratch[E820_MAX_REGIONS * 2];
    uint32_t nNormalized = 0;
    BootStatus st =
        memMapNormalize(e820Regions, nE820, normalized, E820_MAX_REGIONS, &nNormalized, scratch);
    if (st != BOOT_OK) {
        loaderSerialWriteString("loader: memMapNormalize failed: ");
        loaderSerialWriteString(bootStatusString(st));
        loaderSerialWriteString("\n");
        haltForever();
    }

    for (uint32_t i = 0; i < nNormalized; i++) {
        loaderSerialWriteString("loader:   0x");
        writeHex64(normalized[i].base);
        loaderSerialWriteString(" len 0x");
        writeHex64(normalized[i].length);
        loaderSerialWriteString(" ");
        loaderSerialWriteString(memTypeName(normalized[i].type));
        loaderSerialWriteString("\n");
    }

    BootHeap heap;
    st = bootHeapInit(&heap, normalized, nNormalized);
    if (st != BOOT_OK) {
        loaderSerialWriteString("loader: bootHeapInit failed: ");
        loaderSerialWriteString(bootStatusString(st));
        loaderSerialWriteString("\n");
        haltForever();
    }

    uint64_t totalHeapBytes = 0;
    for (uint32_t i = 0; i < heap.count; i++) {
        totalHeapBytes += heap.regions[i].end - heap.regions[i].base;
    }
    loaderSerialWriteString("loader: heap has ");
    loaderSerialWriteUint(heap.count);
    loaderSerialWriteString(" region(s), ");
    loaderSerialWriteUint((uint32_t)(totalHeapBytes >> 20)); /* D-065: shift, not a 64-bit / */
    loaderSerialWriteString(" MiB total\n");
}

void stage2Main(void) {
    loaderSerialInit();
    loaderSerialWriteString("loader: stage2 c environment\n");

    bool has1G = false;
    if (!loaderCpuCheckLongModeFeatures(&has1G)) {
        loaderSerialWriteString("loader: CPU lacks NX; refusing to boot\n");
        haltForever();
    }
    loaderSerialWriteString("loader: CPU has NX");
    loaderSerialWriteString(has1G ? " and PDPE1GB\n" : "\n");

    thunkSelfTest();
    memMapSelfTest();

    haltForever();
}
