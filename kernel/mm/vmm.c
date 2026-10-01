/* See kernel/include/vmm.h. Portable glue: kernel/arch/x86_64/paging.c does the real PML4/CR3/PAT/
 * CR4 work; this file owns the vmmLock and the KVA extent allocator (kva-internal.h) on top of it.
 */
#include "vmm.h"

#include "arch/paging.h"
#include "kva-internal.h"
#include "panic.h"

#include <arch/cpu.h>
#include <stdbool.h>
#include <stdint.h>

static KvaState kvaState;
static bool vmmActive = false;

/* --- lock: IRQ-disable only, same D-081/D-088 single-CPU/IF=0 justification as the pmm's lock
 * (kernel/mm/pmm.c) -- a real spinlock arrives with SMP (M3.4/M3.5). Lock order: vmmLock ->
 * pmmLock (archMapPages/archUnmapPages call into the pmm for table-page allocation while vmmLock
 * is held; nothing here is ever called with pmmLock already held). */
static uint64_t vmmLock(void) {
    return archIrqSave();
}
static void vmmUnlock(uint64_t flags) {
    archIrqRestore(flags);
}

void vmmInit(const BootInfo *bi, const BootMemRegion *map, uint32_t mapCount) {
    archPatInit();
    archPagingBuildKernel(bi, map, mapCount);
    archPagingActivate();
    vmmActive = true; /* before archCpuEnableProtections()/archPagingVerifyWx(): both panic if
                       * called while inactive, and both must actually run against the live
                       * kernel tables, not the (now-unreachable) loader ones */
    archCpuEnableProtections();
    archPagingVerifyWx();
    kvaStateInit(&kvaState, VM_KVA_BASE, VM_KVA_END);
}

bool vmmKernelTablesActive(void) {
    return vmmActive;
}

static bool vmmRangeInKva(uint64_t va, uint64_t size) {
    if (size == 0 || (va % KVA_PAGE_SIZE) != 0 || (size % KVA_PAGE_SIZE) != 0) {
        return false;
    }
    uint64_t end = va + size;
    if (end < va) { /* overflow */
        return false;
    }
    return va >= VM_KVA_BASE && end <= VM_KVA_END;
}

Status vmmMapKernel(uint64_t va, uint64_t pa, uint64_t size, VmmFlags flags) {
    if (!vmmActive) {
        panic("vmmMapKernel: called before vmmInit()");
    }
    if (!vmmRangeInKva(va, size) || (pa % KVA_PAGE_SIZE) != 0) {
        return STATUS_ERR_INVALID;
    }
    uint64_t irqFlags = vmmLock();
    Status st = archMapPages(va, pa, size, flags);
    vmmUnlock(irqFlags);
    return st;
}

Status vmmUnmapKernel(uint64_t va, uint64_t size) {
    if (!vmmActive) {
        panic("vmmUnmapKernel: called before vmmInit()");
    }
    if (!vmmRangeInKva(va, size)) {
        return STATUS_ERR_INVALID;
    }
    uint64_t irqFlags = vmmLock();
    Status st = archUnmapPages(va, size);
    vmmUnlock(irqFlags);
    return st;
}

Status vmmLookupKernel(uint64_t va, uint64_t *outPa, VmmFlags *outFlags) {
    if (!vmmActive) {
        panic("vmmLookupKernel: called before vmmInit()");
    }
    uint64_t irqFlags = vmmLock();
    Status st = archLookupKernel(va, outPa, outFlags);
    vmmUnlock(irqFlags);
    return st;
}

Status vmmKvaAlloc(uint64_t size, uint64_t *outVa) {
    if (!vmmActive) {
        panic("vmmKvaAlloc: called before vmmInit()");
    }
    uint64_t irqFlags = vmmLock();
    Status st = kvaAlloc(&kvaState, size, outVa);
    vmmUnlock(irqFlags);
    return st;
}

void vmmKvaFree(uint64_t va, uint64_t size) {
    if (!vmmActive) {
        panic("vmmKvaFree: called before vmmInit()");
    }
    /* kvaFree() itself also rejects anything outside [VM_KVA_BASE, VM_KVA_END) (its own `base`/
     * `end`), but checking the caller's exact args here first, the same way vmmMapKernel/
     * vmmUnmapKernel do, keeps the bounds check in one obvious place across every vmm entry
     * point rather than relying on kva.c's internal state matching VM_KVA_BASE/END by
     * construction. */
    if (!vmmRangeInKva(va, size)) {
        panicBug("vmm: kvaFree: range outside the KVA region va=0x%llx size=0x%llx",
                 (unsigned long long)va, (unsigned long long)size);
    }
    uint64_t irqFlags = vmmLock();
    Status st = kvaFree(&kvaState, va, size);
    vmmUnlock(irqFlags);
    if (st != STATUS_OK) {
        panicBug("vmm: kvaFree: invalid range va=0x%llx size=0x%llx", (unsigned long long)va,
                 (unsigned long long)size);
    }
}

Status vmmMapMmio(uint64_t pa, uint64_t size, volatile void **outVa) {
    if (!vmmActive) {
        panic("vmmMapMmio: called before vmmInit()");
    }
    uint64_t end = pa + size;
    if (size == 0 || end < pa || end > UINT64_MAX - (KVA_PAGE_SIZE - 1)) {
        return STATUS_ERR_INVALID;
    }
    uint64_t base = pa & ~(uint64_t)(KVA_PAGE_SIZE - 1);
    uint64_t len = ((end + KVA_PAGE_SIZE - 1) & ~(uint64_t)(KVA_PAGE_SIZE - 1)) - base;
    uint64_t va;
    Status st = vmmKvaAlloc(len, &va);
    if (st != STATUS_OK) {
        return st;
    }
    st = vmmMapKernel(va, base, len, VMM_WRITE | VMM_CACHE_UC);
    if (st != STATUS_OK) {
        vmmKvaFree(va, len);
        return st;
    }
    *outVa = (volatile void *)(uintptr_t)(va + (pa - base));
    return STATUS_OK;
}

void vmmUnmapMmio(volatile void *va, uint64_t size) {
    uint64_t a = (uint64_t)(uintptr_t)va;
    uint64_t base = a & ~(uint64_t)(KVA_PAGE_SIZE - 1);
    uint64_t end = a + size;
    if (size == 0 || end < a || end > UINT64_MAX - (KVA_PAGE_SIZE - 1)) {
        panicBug("vmm: vmmUnmapMmio: bad range va=0x%llx size=0x%llx", (unsigned long long)a,
                 (unsigned long long)size);
    }
    uint64_t len = ((end + KVA_PAGE_SIZE - 1) & ~(uint64_t)(KVA_PAGE_SIZE - 1)) - base;
    Status st = vmmUnmapKernel(base, len);
    if (st != STATUS_OK) {
        panicBug("vmm: vmmUnmapMmio: not a vmmMapMmio mapping va=0x%llx size=0x%llx",
                 (unsigned long long)a, (unsigned long long)size);
    }
    vmmKvaFree(base, len);
}
