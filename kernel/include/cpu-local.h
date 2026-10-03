/* Per-CPU data (ARCHITECTURE §7.1, D-190). Each CPU's GS base points at its own CpuLocal; the BSP's
 * is the static cpuLocalBsp (entry.asm loads its GS base before kernelMain runs), an AP's is
 * allocated by the BSP before the AP starts. `cpuLocal()` is one `mov %gs:0` (the `self` field at
 * offset 0), so anything reachable from it is per-CPU without a lookup. Subsystems that own
 * per-CPU state (pmm, slab, timers) hang it off the opaque pointers below. */
#ifndef KERNEL_CPU_LOCAL_H
#define KERNEL_CPU_LOCAL_H

#include "cpu-sync.h"

#include <arch/cpu-local.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CPU_MAX 64 /* the online mask is one uint64_t */

/* The cross-CPU request mailbox of one CPU (D-195): a sender stores its request (on its own stack)
 * in slot[senderId], then sets its bit in `pending`; one IPI later the receiver swaps `pending` to
 * 0 and runs every flagged slot. A sender has at most one outstanding request (it waits for the
 * completion), so one slot per sender is enough and nothing is allocated. */
struct SmpReq;
typedef struct SmpMailbox {
    uint64_t pending;
    struct SmpReq *slot[CPU_MAX];
    uint64_t handled[2];  /* requests run here, by kind: [0] call-function, [1] TLB shootdown */
    uint64_t ipiCount[4]; /* IPIs received on vectors 0xF0..0xF3 */
} SmpMailbox;

typedef struct CpuLocal {
    struct CpuLocal *self; /* MUST stay at offset 0: cpuLocal() reads %gs:0 */
    uint32_t cpuId;        /* dense: BSP = 0, then in bring-up order */
    uint32_t apicId;
    CpuSync sync;
    volatile uint32_t irqDepth; /* nonzero while a hard-IRQ handler runs on this CPU */
    volatile uint32_t bootStage;
    volatile uint32_t stopped;
    void *currentThread; /* NULL until M4 */
    void *idleThread;
    void *runQueue;
    void *pmm; /* per-CPU blobs owned by their subsystems (D-190) */
    void *slab;
    void *timer;
    SmpMailbox mbox;
    ArchCpuLocal arch;
} CpuLocal;
_Static_assert(offsetof(CpuLocal, self) == 0, "CpuLocal.self must be at offset 0");

extern CpuLocal cpuLocalBsp;        /* kernel/core/cpu-local.c */
extern CpuLocal *cpuTable[CPU_MAX]; /* published CPUs, NULL when not online */

/* The calling CPU's CpuLocal. Valid from the first instruction of kernelMain (entry.asm). No
 * locks, IRQ-safe, never fails. Callers that need a stable answer must not be preemptible or
 * migratable (nothing migrates before M4). */
static inline CpuLocal *cpuLocal(void) {
    return archCpuLocal();
}

/* Dense id of the calling CPU. Same context rules as cpuLocal(). */
static inline uint32_t smpThisCpu(void) {
    return cpuLocal()->cpuId;
}

/* The CpuLocal of online CPU `id`, or NULL if `id` is out of range or not online. IRQ-safe. */
CpuLocal *cpuLocalOf(uint32_t id);

/* Fills the BSP's CpuLocal (ids, stack ranges) and publishes cpuTable[0]. Called once from
 * archCpuInitBsp(), before anything that needs a per-CPU stack range. Boot-time only. */
void cpuLocalInitBsp(void);

#endif
