/* x86-specific ktests (ARCHITECTURE §7.1, D-072/D-076): exercise the real GDT/TSS/IDT and
 * exception dispatch end to end. Portable ktests (stack smash, UBSan, symbolize, backtrace) live
 * under kernel/test/ instead. */
#include "trap-frame.h"

#include "ktest.h"
#include "sections.h"

#include <arch/jmp.h>
#include <stdbool.h>
#include <stdint.h>

extern void archKtestInt3Clobber(uint64_t out[14]);
extern void archKtestInt3Resume(void);
extern void archKtestDoubleFault(void);

typedef struct __attribute__((packed)) Dtr {
    uint16_t limit;
    uint64_t base;
} Dtr;

static void readGdtr(Dtr *out) {
    __asm__ volatile("sgdt %0" : "=m"(*out));
}

static void readIdtr(Dtr *out) {
    __asm__ volatile("sidt %0" : "=m"(*out));
}

static uint16_t readCs(void) {
    uint16_t v;
    __asm__ volatile("mov %%cs, %0" : "=r"(v));
    return v;
}

static uint16_t readSs(void) {
    uint16_t v;
    __asm__ volatile("mov %%ss, %0" : "=r"(v));
    return v;
}

static uint16_t readTr(void) {
    uint16_t v;
    __asm__ volatile("str %0" : "=r"(v));
    return v;
}

KTEST(cpu_tables_loaded) {
    Dtr gdtr, idtr;
    readGdtr(&gdtr);
    readIdtr(&idtr);
    KTEST_ASSERT_EQ(gdtr.limit, 8 * 8 - 1);
    KTEST_ASSERT_EQ(idtr.limit, 256 * 16 - 1);
    KTEST_ASSERT_EQ(readTr(), 0x30);
    KTEST_ASSERT_EQ(readCs(), 0x08);
    KTEST_ASSERT_EQ(readSs(), 0x10);

    const uint8_t *gdt = (const uint8_t *)(uintptr_t)gdtr.base;
    KTEST_ASSERT_EQ(gdt[0x30 + 5], 0x8B); /* TSS descriptor access byte: present, DPL0, busy */

    const uint8_t *idt = (const uint8_t *)(uintptr_t)idtr.base;
    KTEST_ASSERT_EQ(idt[8 * 16 + 4] & 0x7, 1);  /* #DF -> IST1 */
    KTEST_ASSERT_EQ(idt[2 * 16 + 4] & 0x7, 2);  /* NMI -> IST2 */
    KTEST_ASSERT_EQ(idt[18 * 16 + 4] & 0x7, 3); /* #MC -> IST3 */
}

KTEST(trap_int3_resumes) {
    static const uint64_t expected[14] = {
        0x1111111111111111ULL, 0x2222222222222222ULL, 0x3333333333333333ULL, 0x4444444444444444ULL,
        0x5555555555555555ULL, 0x6666666666666666ULL, 0x7777777777777777ULL, 0x8888888888888888ULL,
        0x9999999999999999ULL, 0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL, 0xCCCCCCCCCCCCCCCCULL,
        0xDDDDDDDDDDDDDDDDULL, 0xEEEEEEEEEEEEEEEEULL,
    };
    uint32_t bpBefore = archTrapBpCount();
    uint64_t out[14];
    archKtestInt3Clobber(out);
    for (uint32_t i = 0; i < 14; i++) {
        KTEST_ASSERT_EQ(out[i], expected[i]);
    }
    KTEST_ASSERT_EQ(archTrapBpCount(), bpBefore + 1);
    KTEST_ASSERT_EQ(archTrapBpLastRip(), (uint64_t)(uintptr_t)archKtestInt3Resume);
}

KTEST(trap_ud_caught) {
    static const uint8_t ud2[2] = {0x0F, 0x0B};
    KTEST_ASSERT_EQ(archTrapExpect(6, ud2, 2), STATUS_OK);
    __asm__ volatile("ud2");

    ArchTrapRecord rec;
    KTEST_ASSERT(archTrapExpectTake(&rec));
    KTEST_ASSERT_EQ(rec.vector, 6);
    KTEST_ASSERT_EQ(rec.errorCode, 0);
    KTEST_ASSERT_EQ(rec.cs, 0x08);
}

KTEST(trap_pf_reports_cr2) {
    uint64_t g = (uint64_t)(uintptr_t)kernelBootStackBottom - 4096; /* the unmapped guard page */

    static const uint8_t readInsn[3] = {0x48, 0x8B, 0x07}; /* mov (%rdi), %rax */
    KTEST_ASSERT_EQ(archTrapExpect(14, readInsn, 3), STATUS_OK);
    uint64_t v;
    __asm__ volatile("movq (%%rdi), %%rax" : "=a"(v) : "D"(g) : "memory");
    ArchTrapRecord readRec;
    KTEST_ASSERT(archTrapExpectTake(&readRec));
    KTEST_ASSERT_EQ(readRec.vector, 14);
    KTEST_ASSERT_EQ(readRec.cr2, g);
    KTEST_ASSERT_EQ(readRec.errorCode, 0x0);

    static const uint8_t writeInsn[3] = {0x48, 0x89, 0x07}; /* mov %rax, (%rdi) */
    KTEST_ASSERT_EQ(archTrapExpect(14, writeInsn, 3), STATUS_OK);
    __asm__ volatile("movq %%rax, (%%rdi)" : : "a"(v), "D"(g) : "memory");
    ArchTrapRecord writeRec;
    KTEST_ASSERT(archTrapExpectTake(&writeRec));
    KTEST_ASSERT_EQ(writeRec.vector, 14);
    KTEST_ASSERT_EQ(writeRec.cr2, g);
    KTEST_ASSERT_EQ(writeRec.errorCode, 0x2);
}

KTEST(trap_df_on_ist1) {
    ktestArmExpectedPanic(ktestCtx, "exception #DF");
    if (archJmpSave(ktestPanicJmpBuf()) == 0) {
        archKtestDoubleFault();
        KTEST_ASSERT(false); /* unreachable: archKtestDoubleFault() never returns */
    } else {
        uint64_t frame = archTrapDfLastFrame();
        uint64_t bottom = (uint64_t)(uintptr_t)kernelIst1StackBottom;
        uint64_t top = (uint64_t)(uintptr_t)kernelIst1StackTop;
        KTEST_ASSERT(frame >= bottom && frame < top);
    }
}
