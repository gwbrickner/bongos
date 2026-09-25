/* ktests for the kernel's own page tables (ARCHITECTURE §6.3/§23, D-086..D-090, ROADMAP M2.3):
 * the W^X enforcement (not just the boot-time verifier) and the framebuffer's WC PAT encoding. */
#include "kernel-boot.h"
#include "ktest.h"
#include "pte.h"
#include "sections.h"

#include <arch/cpu.h>
#include <arch/paging.h>
#include <arch/trap.h>
#include <stdint.h>

/* `*p = *p` is a genuine write access (so this is a real, not simulated, #PF), but never actually
 * changes the byte even if the fault somehow didn't fire -- the store is read-modify-write with
 * the value unchanged, not that it matters: a #PF aborts the faulting instruction before any
 * write commits (SDM Vol 3A §4.7). */
static void pagingTextWriteTrigger(void *arg) {
    volatile uint8_t *p = (volatile uint8_t *)arg;
    *p = *p;
}

/* ROADMAP M2.3: "a write to the text segment faults". Text is R-X (D-086) -- a write must take a
 * #PF with P=1 (the page exists) and W=1 (a write access), never P=0 (which would mean the
 * mapping is missing entirely, not read-only). */
KTEST(paging_text_write_faults) {
    volatile const uint8_t *target = kernelTextStart + 0x10;
    TrapCatchInfo info;
    bool caught =
        archTrapCatch(TRAP_CATCH_VEC(14), pagingTextWriteTrigger, (void *)(uintptr_t)target, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.vector, 14);
    KTEST_ASSERT_EQ(info.cr2, (uint64_t)(uintptr_t)target);
    KTEST_ASSERT_EQ(info.errorCode & 0x3, 0x3); /* P=1 (present), W=1 (write) */
}

/* A `ret` (0xC3) byte in .data -- NX per D-086, so calling through it must fault on the
 * instruction fetch rather than actually returning. If NX were somehow not enforced, this would
 * just execute the `ret` and return normally, failing the KTEST_ASSERT(caught) below cleanly
 * rather than corrupting anything. */
static uint8_t pagingExecProbe[16] __attribute__((aligned(16))) = {0xC3};

static void pagingDataExecTrigger(void *arg) {
    void (*fn)(void) = (void (*)(void))(uintptr_t)arg;
    fn();
}

/* ROADMAP M2.3: "executing from a data page faults". #PF's error code for an instruction fetch
 * against an NX page is P=1, W=0, I/D=1 (SDM Vol 3A §4.7 Table 4-12), i.e. errorCode & 0x1F ==
 * 0x11. */
KTEST(paging_data_exec_faults) {
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_VEC(14), pagingDataExecTrigger,
                                (void *)(uintptr_t)pagingExecProbe, &info);
    KTEST_ASSERT(caught);
    KTEST_ASSERT_EQ(info.vector, 14);
    KTEST_ASSERT_EQ(info.cr2, (uint64_t)(uintptr_t)pagingExecProbe);
    KTEST_ASSERT_EQ(info.errorCode & 0x1F, 0x11);
}

/* ROADMAP M2.3: "the framebuffer mapping is WC (read the PAT bits back)". Reads the framebuffer's
 * live HHDM leaf PTE and confirms its PAT-index bits (SDM Vol 3A §11.12.3: index = PAT<<2|PCD<<1|
 * PWT) select IA32_PAT's WC entry (D-087's index 1). A boot with no framebuffer (fb.phys == 0) is
 * a vacuous pass here -- vmmInit()'s own "framebuffer ... mapped WC" klog line (grepped by
 * mk/test.mk alongside this test) is what keeps that case from silently hiding a real regression
 * in CI, which always boots with one. */
KTEST(paging_fb_wc) {
    const BootInfo *bi = kernelBootInfo();
    if (bi->fb.phys == 0) {
        return;
    }
    uint64_t va = bi->hhdmBase + bi->fb.phys;
    uint64_t pte = archPagingRawPte(va);
    KTEST_ASSERT((pte & X86_PTE_P) != 0);
    KTEST_ASSERT((pte & X86_PTE_W) != 0);
    KTEST_ASSERT((pte & X86_PTE_NX) != 0);

    uint32_t patIdx =
        (uint32_t)((((pte >> 7) & 1) << 2) | (((pte >> 4) & 1) << 1) | ((pte >> 3) & 1));
    uint64_t pat = archRdmsr(0x277u); /* IA32_PAT */
    uint8_t patEntry = (uint8_t)(pat >> (8 * patIdx));
    KTEST_ASSERT_EQ(patEntry, 0x01); /* D-087: index 1 is WC */
}
