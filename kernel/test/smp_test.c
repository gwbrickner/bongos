/* ktests for per-CPU data and SMP bring-up (M3.5, D-190..). */
#include "acpi.h"
#include "cpu-local.h"
#include "kernel-boot.h"
#include "ktest.h"
#include "preempt.h"
#include "smp.h"

#include <arch/cpu.h>
#include <stdint.h>

#define MSR_GS_BASE        0xC0000101U
#define MSR_KERNEL_GS_BASE 0xC0000102U

/* D-190: the BSP's GS base is its CpuLocal, `self` is at offset 0, the TSS and GDT it runs on are
 * the ones in its own CpuLocal, and cpuSync()/irqDepth live in it. */
KTEST(smp_cpulocal_bsp) {
    CpuLocal *cl = cpuLocal();
    KTEST_ASSERT(cl == &cpuLocalBsp);
    KTEST_ASSERT(cl->self == cl);
    KTEST_ASSERT_EQ(archRdmsr(MSR_GS_BASE), (uint64_t)(uintptr_t)cl);
    KTEST_ASSERT_EQ(archRdmsr(MSR_KERNEL_GS_BASE), 0);
    KTEST_ASSERT_EQ(cl->cpuId, 0);
    KTEST_ASSERT(cpuLocalOf(0) == cl);
    KTEST_ASSERT(cpuLocalOf(CPU_MAX) == NULL);
    KTEST_ASSERT(cpuSync() == &cl->sync);

    uint64_t gdtrBuf[2] = {0, 0};
    __asm__ volatile("sgdt %0" : "=m"(gdtrBuf));
    KTEST_ASSERT_EQ((gdtrBuf[0] >> 16) | (gdtrBuf[1] << 48), (uint64_t)(uintptr_t)cl->arch.gdt);
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    KTEST_ASSERT_EQ(tr, 0x30);
    uint64_t rsp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    KTEST_ASSERT(rsp >= cl->arch.stackBottom && rsp < cl->arch.stackTop);
}

/* Every CPU the MADT lists as enabled (up to `cpus=` and CPU_MAX) came online, each with a dense
 * id, its own CpuLocal and a distinct APIC id the MADT knows. */
KTEST(smp_online_matches_madt) {
    const AcpiInfo *info = acpiGetInfo();
    KTEST_ASSERT(info != NULL && info->madtStatus == STATUS_OK);
    uint32_t enabled = 0;
    for (uint32_t i = 0; i < info->madt.cpuCount; i++) {
        if (info->madt.cpus[i].flags & 1u) {
            enabled++;
        }
    }
    bool present, invalid;
    uint32_t limit = smpParseCpusOption(kernelCmdline(), CPU_MAX, &present, &invalid);
    uint32_t want = enabled < limit ? enabled : limit;
    KTEST_ASSERT_EQ(smpOnlineCount(), want);
    KTEST_ASSERT_EQ(smpOnlineMask(), want >= 64 ? ~0ULL : ((1ULL << want) - 1));

    for (uint32_t id = 0; id < want; id++) {
        CpuLocal *cl = cpuLocalOf(id);
        KTEST_ASSERT(cl != NULL);
        KTEST_ASSERT(cl->self == cl);
        KTEST_ASSERT_EQ(cl->cpuId, id);
        KTEST_ASSERT_EQ(cl->bootStage, SMP_STAGE_ONLINE);
        bool listed = false;
        for (uint32_t i = 0; i < info->madt.cpuCount; i++) {
            listed = listed || (info->madt.cpus[i].apicId == cl->apicId &&
                                (info->madt.cpus[i].flags & 1u) != 0);
        }
        KTEST_ASSERT(listed);
        for (uint32_t other = 0; other < id; other++) {
            KTEST_ASSERT(cpuLocalOf(other)->apicId != cl->apicId);
        }
    }
    KTEST_ASSERT(cpuLocalOf(want) == NULL);
}
