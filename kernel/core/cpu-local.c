/* See kernel/include/cpu-local.h. */
#include "cpu-local.h"

#include "panic.h"
#include "sections.h"

CpuLocal cpuLocalBsp __attribute__((aligned(64)));
CpuLocal *cpuTable[CPU_MAX];

CpuLocal *cpuLocalOf(uint32_t id) {
    if (id >= CPU_MAX) {
        return NULL;
    }
    return __atomic_load_n(&cpuTable[id], __ATOMIC_ACQUIRE); /* an AP publishes it atomically */
}

void cpuLocalInitBsp(void) {
    CpuLocal *cl = &cpuLocalBsp;
    if (cl->self != cl) {
        panic("cpu-local: GS base was not set up by entry.asm");
    }
    cl->cpuId = 0;
    cl->arch.stackBottom = (uint64_t)(uintptr_t)kernelBootStackBottom;
    cl->arch.stackTop = (uint64_t)(uintptr_t)kernelBootStackTop;
    cl->arch.istBottom[0] = (uint64_t)(uintptr_t)kernelIst1Bottom;
    cl->arch.istTop[0] = (uint64_t)(uintptr_t)kernelIst1Top;
    cl->arch.istBottom[1] = (uint64_t)(uintptr_t)kernelIst2Bottom;
    cl->arch.istTop[1] = (uint64_t)(uintptr_t)kernelIst2Top;
    cl->arch.istBottom[2] = (uint64_t)(uintptr_t)kernelIst3Bottom;
    cl->arch.istTop[2] = (uint64_t)(uintptr_t)kernelIst3Top;
    __atomic_store_n(&cpuTable[0], cl, __ATOMIC_RELEASE);
}
