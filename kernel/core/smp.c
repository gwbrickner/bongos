/* The portable half of SMP state: the online mask (D-190). See kernel/include/smp.h. */
#include "smp.h"

#include "atomic.h"

static uint64_t onlineMask = 1; /* the BSP (dense id 0) is online from the start */

uint64_t smpOnlineMask(void) {
    return ATOMIC_LOAD(&onlineMask, MEM_ACQUIRE);
}

uint32_t smpOnlineCount(void) {
    return (uint32_t)__builtin_popcountll(smpOnlineMask());
}

void smpMarkOnline(uint32_t cpuId) {
    ATOMIC_FETCH_OR(&onlineMask, (uint64_t)1 << cpuId, MEM_SEQ_CST);
}
