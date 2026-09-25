/* Portable ktests for symbolize()/backtraceCapture() (ARCHITECTURE §24, D-073). */
#include "backtrace.h"
#include "ktest.h"
#include "sections.h"
#include "symbolize.h"

#include <arch/cpu.h>
#include <stdint.h>

__attribute__((noinline)) static void ktestSymbolizeTarget(void) {
    __asm__ volatile("");
}

KTEST(symbolize_known_function) {
    uint64_t addr = (uint64_t)(uintptr_t)ktestSymbolizeTarget + 1;
    SymbolInfo sym;
    KTEST_ASSERT_EQ(symbolize(addr, &sym), STATUS_OK);
    KTEST_ASSERT_EQ(sym.start, (uint64_t)(uintptr_t)ktestSymbolizeTarget);
    KTEST_ASSERT(sym.name[0] != '\0');

    SymbolInfo unused;
    KTEST_ASSERT_EQ(symbolize(0x1000, &unused), STATUS_ERR_NOT_FOUND);
    KTEST_ASSERT_EQ(symbolize((uint64_t)(uintptr_t)kernelTextEnd, &unused), STATUS_ERR_NOT_FOUND);
}

__attribute__((noinline)) static uint32_t ktestBtLevel3(uint64_t *pcs, uint32_t max) {
    uint32_t n = backtraceCapture(archFramePointer(), pcs, max);
    __asm__ volatile(""); /* not a tail call: keeps this frame's own return address meaningful */
    return n;
}

__attribute__((noinline)) static uint32_t ktestBtLevel2(uint64_t *pcs, uint32_t max) {
    uint32_t n = ktestBtLevel3(pcs, max);
    __asm__ volatile("");
    return n;
}

__attribute__((noinline)) static uint32_t ktestBtLevel1(uint64_t *pcs, uint32_t max) {
    uint32_t n = ktestBtLevel2(pcs, max);
    __asm__ volatile("");
    return n;
}

KTEST(backtrace_walks_chain) {
    uint64_t pcs[8];
    uint32_t n = ktestBtLevel1(pcs, 8);
    KTEST_ASSERT(n >= 2);

    /* pcs[0] is ktestBtLevel3's return address (into its caller, ktestBtLevel2); pcs[1] is
     * ktestBtLevel2's return address (into ktestBtLevel1). Looked up at pc-1 per D-073's
     * noreturn-call convention (harmless here too, since these calls aren't the last instruction
     * of their function anyway). */
    SymbolInfo sym;
    KTEST_ASSERT_EQ(symbolize(pcs[0] - 1, &sym), STATUS_OK);
    KTEST_ASSERT(sym.start == (uint64_t)(uintptr_t)ktestBtLevel2);

    KTEST_ASSERT_EQ(symbolize(pcs[1] - 1, &sym), STATUS_OK);
    KTEST_ASSERT(sym.start == (uint64_t)(uintptr_t)ktestBtLevel1);
}
