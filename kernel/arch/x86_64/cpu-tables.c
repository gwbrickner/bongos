/* Builds and installs the real per-CPU GDT/TSS/IDT (ARCHITECTURE §7.1, D-072). BSP-only in M2.1
 * (one static CpuTables instance); M3.5 moves CpuTables into CpuLocal for the APs, unchanged. */
#include "panic.h"
#include "sections.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The 64-bit TSS (SDM Vol 3A, "64-Bit TSS Format"). Packed for an exact 104-byte size (an
 * `aligned` attribute here would instead pad sizeof() up to that alignment, which is wrong: the
 * TSS descriptor's limit must be sizeof(Tss)-1 exactly, 0x67). CpuTables (below), which embeds
 * this as its last member, gets the alignment big enough that the TSS can never cross a page. */
typedef struct __attribute__((packed)) Tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopbOffset;
} Tss;

_Static_assert(sizeof(Tss) == 104, "Tss must be 104 bytes (SDM 64-bit TSS format)");
_Static_assert(offsetof(Tss, rsp) == 0x04, "Tss.rsp must sit at offset 0x04");
_Static_assert(offsetof(Tss, ist) == 0x24, "Tss.ist must sit at offset 0x24");
_Static_assert(offsetof(Tss, iopbOffset) == 0x66, "Tss.iopbOffset must sit at offset 0x66");

/* One 64-bit interrupt-gate descriptor (SDM Vol 3A, "64-Bit IDT Gate Descriptors"). */
typedef struct __attribute__((packed)) IdtGate {
    uint16_t offsetLow;
    uint16_t selector;
    uint8_t ist;
    uint8_t typeAttr;
    uint16_t offsetMid;
    uint32_t offsetHigh;
    uint32_t reserved;
} IdtGate;

_Static_assert(sizeof(IdtGate) == 16, "IdtGate must be 16 bytes");

typedef struct __attribute__((packed)) Dtr {
    uint16_t limit;
    uint64_t base;
} Dtr;

_Static_assert(sizeof(Dtr) == 10, "Dtr (GDTR/IDTR image) must be 10 bytes");

#define IDT_VECTOR_COUNT        256
#define IDT_EXCEPTION_COUNT     32
#define IDT_TYPE_INTERRUPT_GATE 0x8E /* present, DPL0, 64-bit interrupt gate */
#define GDT_SEL_KERNEL_CODE     0x08
#define GDT_SEL_TSS             0x30
#define GDT_QWORD_COUNT         8 /* null, code, data, user32-placeholder, user data, user code, TSS x2 */

/* Aligned to 256 (a power of two, >= sizeof(CpuTables) = 168): since 4096 is a multiple of 256,
 * an instance can never straddle a page boundary, so the embedded Tss (which must not cross a
 * page, SDM Vol 3A) never can either. */
typedef struct __attribute__((aligned(256))) CpuTables {
    uint64_t gdt[GDT_QWORD_COUNT];
    Tss tss;
} CpuTables;

static CpuTables bspTables;
static IdtGate idt[IDT_VECTOR_COUNT] __attribute__((aligned(4096)));
static bool cpuTablesInitialized = false;

extern const uint8_t archIsrStubs[]; /* isr.asm: 32 stubs, 16-byte stride */
#define ISR_STUB_STRIDE 16

void archLoadGdt(const void *gdtr);
void archLoadTr(uint16_t selector);
void archLoadIdt(const void *idtr);

static void idtSetGate(uint32_t vector, uint64_t handler, uint8_t istIndex) {
    idt[vector].offsetLow = (uint16_t)(handler & 0xFFFF);
    idt[vector].selector = GDT_SEL_KERNEL_CODE;
    idt[vector].ist = istIndex & 0x7;
    idt[vector].typeAttr = IDT_TYPE_INTERRUPT_GATE;
    idt[vector].offsetMid = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].offsetHigh = (uint32_t)(handler >> 32);
    idt[vector].reserved = 0;
}

static uint8_t istIndexForVector(uint32_t vector) {
    switch (vector) {
        case 8: /* #DF */
            return 1;
        case 2: /* NMI */
            return 2;
        case 18: /* #MC */
            return 3;
        default:
            return 0; /* the current stack */
    }
}

static void buildGdt(CpuTables *t) {
    t->gdt[0] = 0;                     /* null */
    t->gdt[1] = 0x00AF9B000000FFFFULL; /* 0x08 kernel code64: P DPL0 type 0xB L=1 G=1 */
    t->gdt[2] = 0x00CF93000000FFFFULL; /* 0x10 kernel data */
    t->gdt[3] = 0;                     /* 0x18 user code32 placeholder: null on purpose (D-072)
                                        * -- loading it always #GPs, so ring 3 can never reach
                                        * compatibility mode; it only reserves the STAR slot. */
    t->gdt[4] = 0x00CFF3000000FFFFULL; /* 0x20 user data, DPL3 */
    t->gdt[5] = 0x00AFFB000000FFFFULL; /* 0x28 user code64, DPL3 */

    uint64_t base = (uint64_t)(uintptr_t)&t->tss;
    uint64_t limit = sizeof(Tss) - 1; /* 0x67 */
    t->gdt[6] =
        limit | ((base & 0xFFFFFFULL) << 16) | (0x89ULL << 40) | (((base >> 24) & 0xFFULL) << 56);
    t->gdt[7] = base >> 32; /* upper half: bits 44:40 (the type field here) must stay 0 */
}

static void buildTss(CpuTables *t) {
    t->tss.reserved0 = 0;
    for (uint32_t i = 0; i < 3; i++) {
        t->tss.rsp[i] = 0; /* M4 sets rsp0 per thread; unused at CPL0-only M2.1 */
    }
    t->tss.reserved1 = 0;
    for (uint32_t i = 0; i < 7; i++) {
        t->tss.ist[i] = 0;
    }
    t->tss.ist[0] = (uint64_t)(uintptr_t)kernelIst1StackTop; /* IST1 = #DF */
    t->tss.ist[1] = (uint64_t)(uintptr_t)kernelIst2StackTop; /* IST2 = NMI */
    t->tss.ist[2] = (uint64_t)(uintptr_t)kernelIst3StackTop; /* IST3 = #MC */
    t->tss.reserved2 = 0;
    t->tss.reserved3 = 0;
    t->tss.iopbOffset = sizeof(Tss); /* >= TSS limit: no I/O permission bitmap */
}

static void buildIdt(void) {
    for (uint32_t v = 0; v < IDT_VECTOR_COUNT; v++) {
        idt[v] = (IdtGate){0}; /* typeAttr=0 -> not present */
    }
    for (uint32_t v = 0; v < IDT_EXCEPTION_COUNT; v++) {
        uint64_t handler = (uint64_t)(uintptr_t)archIsrStubs + (uint64_t)v * ISR_STUB_STRIDE;
        idtSetGate(v, handler, istIndexForVector(v));
    }
}

/* See trap-frame.h. */
void archCpuTablesInit(void) {
    if (cpuTablesInitialized) {
        panic("archCpuTablesInit: called twice");
    }
    cpuTablesInitialized = true;

    buildTss(&bspTables);
    buildGdt(&bspTables);

    Dtr gdtr = {sizeof(bspTables.gdt) - 1, (uint64_t)(uintptr_t)bspTables.gdt};
    archLoadGdt(&gdtr);
    archLoadTr(GDT_SEL_TSS);

    buildIdt();
    Dtr idtr = {sizeof(idt) - 1, (uint64_t)(uintptr_t)idt};
    archLoadIdt(&idtr);
}
